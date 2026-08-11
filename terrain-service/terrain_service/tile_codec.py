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
#: The per-tile basin registry (docs/water-system-architecture.md P1). A tiny
#: flat table, not a plane: tens of rows at 32 B, so a tile grows by ~1 KB
#: against 26.6 MB of compressed elevation.
SECTION_BASIN_TABLE = 5
#: The water plane (watershed plan P2, bake_ver 9). Same block machinery and
#: element width as elevation, and the same block index -- but the VALUES are
#: water DEPTH above the tile's own quantised bed, not an absolute surface.
#: See WATER_DEPTH_LSB_MM for the three measured reasons.
SECTION_WATER_INDEX = 6
SECTION_WATER_DATA = 7
#: Headwater points (water re-architecture Phase 1, bake_ver 24): the cells
#: where drawn water STARTS, with the discharge at each. A flat table like the
#: basin registry and for the same reason -- it is a few thousand points, not a
#: plane -- and it is the faucet list the PBF solver spawns from. The bake has
#: computed this mask since bake_ver 9 (`water.water_head_mask`) and deleted it
#: unused at the end of B6 ever since.
SECTION_HEADWATERS = 8
#: The bathymetry pair (water appearance plan, bake_ver 27): per-cell lake
#: DEPTH and SIGNED DISTANCE TO SHORE. Two planes through the same block
#: machinery as elevation and water, because both are consumed per-pixel by
#: the water material and neither can be derived from a table row.
#:
#: WHY THE CLIENT CANNOT JUST COMPUTE THESE. It already re-derives a lake's
#: footprint from four wire numbers, and that is right for a footprint -- it is
#: a flood fill over data the client already holds. Depth and shore distance
#: are not: depth needs the datum minus ground at every cell, and distance
#: needs a Euclidean transform over the whole basin, which is a global
#: operation over a region that can be 2.5 million cells and does not
#: decompose per column. The bake does it once; the client fetches a block.
#:
#: WHY NOT ONE PLANE WITH TWO CHANNELS. `_encode_plane` predicts along a
#: scanline, and the two fields have unrelated gradients -- interleaving them
#: would hand the MED predictor a sawtooth and cost more than the second index.
SECTION_BATHY_DEPTH_INDEX = 9
SECTION_BATHY_DEPTH_DATA = 10
SECTION_BATHY_SHORE_INDEX = 11
SECTION_BATHY_SHORE_DATA = 12

FLAG_FLOW_PRESENT = 1 << 0
FLAG_BASINS_PRESENT = 1 << 1
#: bake_ver 9: SECTION_WATER_* is present. Set even when the plane is entirely
#: dry, for the same reason FLAG_BASINS_PRESENT is set on an empty table: "this
#: tile was surveyed and carries no rivers" and "this tile predates the water
#: plane" are different facts, and a client that conflated them would draw no
#: rivers in a world that has them and never be able to tell why.
FLAG_WATER_PRESENT = 1 << 2
#: bake_ver 24: SECTION_HEADWATERS is present. Set even when the tile has no
#: heads at all, for the third time and the same reason: "surveyed, no reaches
#: start here" and "baked before headwaters existed" are different facts.
#:
#: ADDING A FLAG BIT IS A HARD BREAK BY DESIGN. Both parsers refuse an unknown
#: bit (see `decode_v2` and tilestore.h `kFineFlagsKnown`), so an old client
#: handed one of these tiles refuses it loudly instead of drawing a world with
#: half the water missing. That is exactly why this rides a BAKE_VERSION roll
#: (23 -> 24): the product namespace changes with it, so an old client never
#: meets a new tile under an id it thinks it understands.
FLAG_HEADS_PRESENT = 1 << 3
#: bake_ver 27: SECTION_BATHY_* are present. Set even on a tile with no lakes,
#: for the fourth time and the same reason as the three above -- "surveyed, no
#: basins here" and "baked before bathymetry existed" are different facts, and
#: only the flag can tell them apart. One bit covers BOTH planes: they are
#: computed together from one extent pass and there is no case where a tile
#: would carry depth without shore distance or the reverse.
FLAG_BATHY_PRESENT = 1 << 4

# --- SECTION_WATER_* payload: DEPTH ABOVE THE QUANTISED BED ----------------
#
# The plane stores water DEPTH over each pixel, int16 at a 10 mm LSB, with -1
# for dry. NOT the absolute water surface, and the three reasons are all
# measured. The first one answers the objection everybody raises.
#
# 1. THE CANCELLATION, which is why depth does not inherit the bed's ripples.
#    The obvious objection to storing depth is that the bed is quantised to
#    100 mm and carries the meso band and the B1 roughness, so water = bed +
#    depth would ripple by things water does not do. It does not, because the
#    bake computes the depth against the QUANTISED bed rather than the true
#    one:
#
#        bake:    depth_q = round((water_true - bed_q) / 10)
#        client:  water   = bed_q + depth_q * 10      = water_true +/- 5 mm
#
#    bed_q is exactly what the client reconstructs, so the bed's own
#    quantisation error, its meso band and its roughness all CANCEL: whatever
#    the bed does, the stored depth absorbs the complement of it. Measured on a
#    bed varying by 200 mm under a reach: reconstruction error 4.0 mm against a
#    half-LSB of 5.0. (The same trick the elevation plane uses, run backwards.)
#
# 2. ONLY DEPTH FITS. Absolute water at a 10 mm LSB spans +/-327 m in int16
#    while a single tile spans up to 6.5 km of relief -- so an absolute plane
#    at a fine LSB is not representable at all, and at the elevation plane's
#    own 100 mm LSB it is a visible staircase: measured along-flow on the
#    shipped world, 53.73% of downstream steps on genuine river gradients have
#    a drop under 100 mm, so more than half the network could not descend.
#    Depth is BOUNDED by the channel-depth cap -- 18.75 m of water at 3/4 of a
#    25 m channel -- so 10 mm in int16 leaves 17x headroom.
#
# 3. WHY 10 mm AND NOT 1 mm. Measured along-flow on genuine gradients (p50
#    47.1 m/km, after excluding the 58% of river cells whose "gradient" is the
#    epsilon-fill floor and is not terrain): at a 100 mm LSB 53.73% of steps
#    are unrepresentable, at 10 mm 0.00%, at 1 mm also 0.00%. 1 mm buys nothing
#    and costs 10x the headroom. Measured on a gentle reach falling 2 mm/px:
#    reconstructed steps are 0 or -10 mm, against -100 mm treads (one whole
#    voxel at a time) for an absolute plane at the elevation LSB.
#
# WHAT THIS BUYS BESIDES SMOOTHNESS, and it is the property to protect: the
# water surface inherits the terrain's long profile automatically. Gentle where
# the bed is gentle, a genuine fall where the bed falls, with no special case
# for a waterfall -- Q sets depth and width, the ground sets the gradient. The
# world's steepest along-flow drop today is 32.8 m over 30 m (47.6 deg) and
# 0.40% of river cells exceed 45 deg; nothing exceeds 63 deg, so a vertical
# free-fall does not exist in this terrain YET. That is a terrain question
# (knickpoints need uplift or resistant lithology, task #20), not an encoding
# one, and this encoding does not preclude it: a 30 m drop across one fine
# pixel is 3,000 LSB of a 32,767 range.
#
# And the SHORELINE improves rather than degrades. A boolean wet mask puts the
# waterline on a 1.875 m pixel edge -- which is what "sharp, rectangular,
# square edges" looks like in plan view. A continuous depth field lets the
# client find where depth crosses zero SUB-PIXEL, so the shoreline lands on a
# contour instead of on a raster step.

#: Millimetres per stored depth LSB. See point 3 above.
WATER_DEPTH_LSB_MM = 10

#: Dry. -1 rather than INT16_MIN so it is a value a DEPTH can never take (depth
#: is non-negative by construction: water stands above its own bed), which
#: makes the sentinel test "< 0" rather than an equality against a magic
#: number, and leaves the whole positive range for real depths.
#:
#: water_depth_control_points REFUSES a wet cell that quantises to a negative
#: depth rather than clamping it -- a silent clamp would be a wet cell reading
#: dry to every client forever. Unreachable on real terrain: the shallowest
#: head the law can produce is 0.22 m, 22 LSB clear of the sentinel.
WATER_DRY_DEPTH = -1

#: Largest storable depth, metres. int16 max at the 10 mm LSB.
WATER_MAX_DEPTH_M = 32767 * WATER_DEPTH_LSB_MM / 1000.0

# --- THE LEVEL BAND: a water LEVEL for DRY cells near water -----------------
#
# THE COMPLAINT THIS ANSWERS, in the owner's terms: the waterline meets the bank
# in blocky multi-metre steps. It has to, today. The plane says WET or DRY at a
# 1.875 m pitch, so the client can only put the shoreline on a pixel edge -- and
# the ground it actually DRAWS is the amplified 10 cm surface, which rises and
# falls by metres inside one of those pixels. The bake and the client are
# answering the same question at pitches 18.75x apart, and the staircase is the
# difference.
#
# The fix is not more resolution. It is to stop shipping the ANSWER and ship the
# QUESTION: give a dry pixel near water the LOCAL WATER LEVEL, and let the client
# resolve `ground(voxel) < level` at its own 10 cm pitch, against the ground it
# is drawing. The waterline then lands on a contour of the amplified surface
# instead of on a raster step, and it costs no new plane.
#
# THE ENCODING, and why it is negatives rather than a new section:
#
#     v >= 0                 wet; depth = v * WATER_DEPTH_LSB_MM   (UNCHANGED)
#     -32767 <= v <= -2      DRY, and the local water level is
#                            reconstructedGround + v * WATER_DEPTH_LSB_MM
#     v == -1                no level (LEGACY dry; never emitted by a new
#                            encoder, still read as "no level" forever)
#     v == -32768            no level (what a new encoder emits)
#
# EVERY EXISTING READER TESTS `< 0` FOR DRY. Swept across voxel-core, ue-project
# and terrain-service before this landed: the single production decode path is
# `FineTile::waterMmFromDepth` (voxelcore/tilestore.h), `if (depthCp < 0) return
# kNoWaterMm;`, its Python mirror `water_surface_mm_from_depth` below is
# `d >= 0`, and the two constant-block fast paths in bench/farwaterprobe.cpp are
# `constCp < 0 => whole block dry`. Nothing anywhere tests `== -1`, nothing
# reconstructs a depth from a negative, and nothing averages or lerps a raw
# control point. So a band value reads as DRY on an old client, bit for bit.
# That property is the entire reason the band is encoded as negatives in the
# existing plane instead of as a fourth section -- an old client is not merely
# tolerant of the new bytes, it is UNAFFECTED by them.
#
# WHY -2 AND NOT -1 IS THE TOP OF THE BAND. -1 has to keep meaning "no level"
# for every tile already baked, so it cannot also mean "level is 10 mm below the
# ground". -2 costs 10 mm of a quantity that is about to be compared against a
# 100 mm voxel.

#: "This cell is dry and this tile has no level for it." What a level-aware
#: encoder emits outside the band. ``WATER_DRY_DEPTH`` (-1) means the same thing
#: and is what every tile baked before the band carries; a reader must treat the
#: two identically.
#:
#: MEASURED COST OF CHOOSING -32768 OVER -1, on shipped tile (-4,-4) at
#: bake_ver 15, water plane only: the sentinel change ALONE (no band at all)
#: takes the plane from 0.645 MB to 0.858 MB under CODEC_ZSTD and from 62.1 MB
#: to 124.3 MB under CODEC_RAW -- because a -32768 sea beside a +2000 river
#: makes MED residuals that no longer fit 16 bits, flipping every coded block to
#: ``resid_bits=32``. With the band on, emitting -1 instead would cost 1.646 MB
#: rather than 2.215 MB (zstd) and 63.9 MB rather than 123.8 MB (raw) -- under
#: CODEC_RAW, which is what every tile in the cache is written with today, the
#: sentinel choice is the ENTIRE cost of the feature: 1.03x with -1 against
#: 1.99x with -32768, and the band itself is nearly free either way. The
#: THE DISTINCT SENTINEL IS PROBABLY NOT WORTH IT -- MEASURED, NOT YET TAKEN.
#: It buys exactly one thing: a tile can say "I was surveyed for levels and this
#: cell has none" as against "I predate levels entirely". `bake_ver` already
#: answers that, and it is on every tile header. The cost is not marginal: on
#: CODEC_RAW, which is what every tile in the cache is written with today, the
#: sentinel choice is the ENTIRE cost of the feature -- 1.99x with -32768
#: against 1.03x with -1 -- because a -32768 sea beside a +2000 river makes MED
#: residuals that no longer fit 16 bits and flips every coded block to
#: resid_bits=32. That is roughly 13 GB -> 26 GB across the cache to encode an
#: answer the header already gives.
#:
#: NOT CHANGED YET, DELIBERATELY. Two tests pin -32768 and a concurrent rewrite
#: of the lateral rule was touching the same module; making a format decision
#: while two things move is how you end up debugging both. The feature ships
#: OFF, so nothing pays this cost today. Change this line, update
#: test_level_band_constants_are_product_only and
#: test_level_band_round_trips_through_the_codec, and re-measure before the
#: first bake that enables water_level_plane_enabled.
WATER_NO_LEVEL = -32768

#: Inclusive bounds of the level band. A stored ``v`` in ``[MIN, MAX]`` means
#: "dry, and the water level here is ``ground + v * WATER_DEPTH_LSB_MM``".
#:
#: THE FLOOR IS NOT COSMETIC AND MUST NOT BE RAISED TO THE BAND WIDTH. It is
#: tempting to clamp at -``WATER_LEVEL_BAND_MM``/LSB (= -240) since a level more
#: than the band below the ground can never produce a voxel -- and on the
#: pixel's OWN ground that is true. It is false once the band's dilation is in
#: play: a steep pixel is admitted because a neighbour two pixels away stands at
#: the level, and clamping its stored value to -240 would tell the client the
#: water is 2.4 m below a ground much further up than that. The client's carrier
#: warp lets that column read the neighbour's low ground, and it would then fill
#: the difference. MEASURED on shipped tiles (-4,-4) and (-5,-5) at the shipped
#: dilation, the worst |level - ground| over banded cells is 22.2 m and 26.0 m
#: -- so the clamp would invent up to 24 m of water on a cliff. Clamping only
#: ever RAISES a level, and raising a level adds water. Keep the full range.
#:
#: The int16 floor itself is never reached on real terrain: clamps at
#: WATER_LEVEL_MIN_CP were 0 of 674,421 and 0 of 1,635,171 banded cells on those
#: two tiles. It is a representability bound, not a working limit.
WATER_LEVEL_MIN_CP = -32767
WATER_LEVEL_MAX_CP = -2

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
#
# VERSION 2 (water re-architecture Phase 1, bake_ver 24) APPENDS to the row and
# changes nothing in front of it: bytes 0..31 of a v2 row are positionally
# identical to a whole v1 row, so the two layouts differ only by a suffix and
# `entry_bytes` is what tells them apart. That is the mechanism this header was
# built with in mind ("`entry_bytes` is what lets a later revision add a field
# without a third section id") and it is used exactly as written. Both decoders
# here and in tilestore.cpp accept BOTH, because v1 tiles are on disk today.
BASIN_TABLE_VERSION_V1 = 1
BASIN_TABLE_VERSION_V2 = 2
BASIN_TABLE_VERSION = BASIN_TABLE_VERSION_V2
_BASIN_TABLE_HEADER = struct.Struct("<HHI")

# One v1 basin row, 32 bytes:
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
BASIN_ENTRY_BYTES_V1 = 32
#: The v1 name, kept pointing at the v1 size. NOT re-aimed at v2: it is quoted
#: by the committed v1 fixture generator and by voxelcore's `kBasinEntryBytes`,
#: and a constant that silently changed size under an unchanged name is how a
#: reader ends up parsing 80-byte rows out of a 32-byte stream.
BASIN_ENTRY_BYTES = BASIN_ENTRY_BYTES_V1
_BASIN_RESERVED5 = bytes(5)

# The v2 tail, 48 more bytes immediately after those 32 (row total 80):
#
#   global_id u64 | capacity_l u64 | floor_mm i32
#   world_bbox 4*i32 | world_outlet 2*i32 | span_flags u8 | reserved u8[3]
#
# WHAT EACH ONE IS FOR, since a wire field with no consumer is how `spill_mm`
# and `outlet_px` sat unread for four bake versions:
#
# * `global_id` -- the basin's FLOOR CELL in absolute world fine pixels, packed
#   (`bake.basins.global_basin_id`). Two tiles that both register one physical
#   basin write the same id with no cross-tile communication in the bake. It is
#   a packing, not a hash, so there is no collision argument to make and the id
#   reads back as a place you can go and look at.
#
# * `capacity_l` -- LITRES (dm^3) of headroom between `surface_mm` and
#   `spill_mm`. The unit is picked by an overflow bound, not by taste:
#     - realistic worst case, wet alpine block: kept basin coverage tops out at
#       2.4% of a 23,593 ha tile = 566 ha, against a 46 m deepest spill, so
#       5.66e6 m^2 * 46 m = 2.6e8 m^3 = 2.6e11 litres;
#     - absurd worst case, the whole padded domain 46 m deep: 2.99e8 m^2 *
#       6553.4 m (the elevation plane's own int16 span at 100 mm) = 1.96e12
#       m^3 = 1.96e15 litres.
#   u32 dies immediately -- 4.295e9 litres is 4.3e6 m^3, which is 0.76 m of
#   water over that same 566 ha lake -- and u32 in m^3 survives the realistic
#   case but not the absurd one. u64 litres clears the absurd case by 9,400x
#   and keeps a 1-litre LSB, so the ledger never has to reason about rounding.
#
# * `floor_mm` -- the deepest cell's elevation, absolute mm. v1 shipped the
#   spill and the surface but never the bottom, so a client could not turn a
#   volume into a level (it needs floor, surface and A(h)) nor tell a 2 m pond
#   from a 46 m lake standing at the same surface.
#
# * `world_bbox` / `world_outlet` -- the component's UNCLIPPED extent and its
#   spill saddle, absolute world fine pixels. The u16 `bbox_px`/`outlet_px` in
#   front are clipped and clamped into the tile so a client can index its own
#   plane with them; for a tile-spanning basin those are a local VIEW and these
#   are the truth. The client's union rule reads these: two rows are one lake
#   when their ids match, or when their world bboxes overlap and their spills
#   agree -- see the design note in bake/basins.py:global_basin_id for why the
#   second clause has to exist.
#
# * `span_flags` bit0 -- this row's extent leaves this tile, i.e. "expect a
#   partner row in a neighbour". Derivable from world_bbox vs the tile, but a
#   client that has not fetched the neighbour needs to know it is looking at
#   half a lake without doing that arithmetic on every row of every tile.
_BASIN_ENTRY_V2_TAIL = struct.Struct("<2Qi6iB3s")
BASIN_ENTRY_BYTES_V2 = 80
_BASIN_RESERVED3 = bytes(3)
#: `span_flags` bit0: the basin's extent leaves this tile's interior.
BASIN_SPAN_CROSSES_TILE = 1 << 0
#: `global_id`'s coordinate packing, mirroring `bake.basins`. Two biased 31-bit
#: fields under a constant bit-62 tag; bit 63 stays CLEAR and the id is never
#: 0, both required by voxelcore/basinledger.h's `BasinId` (it tags v1
#: tile-local keys with bit 63 and reserves 0 as "not a basin").
BASIN_ID_AXIS_BIAS = 1 << 30
BASIN_ID_TAG = 1 << 62
#: Litres per cubic metre -- the capacity field's LSB.
CAPACITY_L_PER_M3 = 1000
#: u64 ceiling, asserted at pack time rather than trusted (see the bound above).
CAPACITY_L_MAX = (1 << 64) - 1

_BASIN_ENTRY_BYTES_BY_VERSION = {
    BASIN_TABLE_VERSION_V1: BASIN_ENTRY_BYTES_V1,
    BASIN_TABLE_VERSION_V2: BASIN_ENTRY_BYTES_V2,
}

# --- SECTION_HEADWATERS layout (water re-architecture Phase 1) --------------
#
# Same shape as the basin table -- u16 version, u16 entry_bytes, u32 count --
# for the same reason, and deliberately NOT a plane: a head is a point.
#
#   px u16 | py u16 | q_m3_yr u32          (8 bytes, tile-interior pixels)
#
# WHY u32 AND WHAT IT COSTS. Q at a head is small by construction (a head is
# the most upstream drawable cell), but a reach that ENTERS the tile's padded
# domain from outside has no donor inside it and is a head too, carrying the
# full trunk discharge. The largest such value seen on the shipped world is
# 2.3e8 m^3/yr, against a u32 ceiling of 4.295e9 -- 18x of headroom, and 136
# m^3/s in the units hydrology uses, which is a large river but not a Rhine.
# The encoder REFUSES a head above the ceiling rather than saturating: a
# saturated faucet rate would be a plausible number that silently understates
# a river, and the bake failing loudly is the cheaper outcome.
HEADWATER_TABLE_VERSION = 1
_HEADWATER_TABLE_HEADER = struct.Struct("<HHI")
_HEADWATER_ENTRY = struct.Struct("<2HI")
HEADWATER_ENTRY_BYTES = 8
HEADWATER_Q_MAX = (1 << 32) - 1
#: The numpy view of one row, for the vectorised encode/decode path -- a tile
#: can carry thousands of heads and a Python loop over struct.pack is the kind
#: of avoidable cost that turns into a bake regression nobody attributes.
_HEADWATER_DTYPE = np.dtype([("x", "<u2"), ("y", "<u2"), ("q", "<u4")])

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
    `water_cp`, if given, is the P2 water plane and sets flags bit2 -- int16
    water DEPTH above this tile's own quantised elevation lattice at a
    `WATER_DEPTH_LSB_MM` LSB, negative where dry. An ALL-DRY plane is
    likewise not None, for the identical reason. A negative is not necessarily
    just "dry": see `WATER_NO_LEVEL` for the level band that `-32767..-2`
    carries, which every reader that tests `< 0` already handles correctly.
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
    water_cp: "np.ndarray | None" = None   # (size,size) int16 DEPTH cp, P2
    #: Headwater points (Phase 1, bake_ver 24), sets flags bit3. An EMPTY list
    #: is not None, for the third time and the same reason as `basins` and
    #: `water_cp`: "surveyed, no reaches start here" is a fact.
    heads: "list[HeadEntry] | None" = None
    #: Bathymetry (bake_ver 27), sets flags bit4. Both or neither -- see
    #: FLAG_BATHY_PRESENT. int16 like the other two planes; units are
    #: basins.BATHY_DEPTH_LSB_MM and BATHY_SHORE_LSB_MM respectively.
    bathy_depth: "np.ndarray | None" = None   # (size,size) int16, 10 mm, -1 dry
    bathy_shore: "np.ndarray | None" = None   # (size,size) int16, 100 mm, signed
    #: Which SECTION_BASIN_TABLE layout to WRITE. Explicit rather than inferred
    #: from whether the rows carry v2 fields: "these rows have no identity" and
    #: "this tile is a v1 tile" are different statements, and a v2 bake that
    #: quietly emitted a v1 table because one row was built wrong is exactly
    #: the silent downgrade `_pack_v2_tail` refuses. Only the v1 fixture
    #: generator sets this.
    basin_table_version: int = BASIN_TABLE_VERSION

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
        if self.water_cp is not None:
            assert self.water_cp.shape == (self.size, self.size)
            assert self.water_cp.dtype == np.int16
        # BOTH OR NEITHER, checked rather than documented: one flag bit covers
        # the pair, so a tile carrying only one of them would set the flag and
        # then fail the section-agreement check inside the decoder, which is a
        # far worse place to find out.
        assert (self.bathy_depth is None) == (self.bathy_shore is None), (
            "bathy_depth and bathy_shore must be given together (one flag bit "
            "covers both planes)"
        )
        for _name, _p in (("bathy_depth", self.bathy_depth),
                          ("bathy_shore", self.bathy_shore)):
            if _p is not None:
                assert _p.shape == (self.size, self.size), f"{_name} shape"
                assert _p.dtype == np.int16, f"{_name} dtype"
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


def water_depth_control_points(water_m, ground_m, base_offset_mm: int,
                               quant: int, level_m=None) -> np.ndarray:
    """Water surface (m, NaN = dry) + the tile's OWN elevation cp -> depth cp.

    THE BED IS THE SAMPLE FIELD, in metres -- the same ``elevation_m`` this
    tile encodes, NOT the control lattice. That distinction cost a revision and
    is worth stating precisely, because the obvious reading is wrong twice over.

    ``cp`` is a B-SPLINE CONTROL LATTICE, not the surface: the prefilter moves a
    control point up to 5.6 m away from the sample it interpolates (measured,
    p99 of |cp - sample| = 4.4 m on this world). Referencing the depth to
    ``control_points_to_mm(cp)`` therefore references it to something that is
    not the bed at all, and on real terrain it produces water below its own bed.

    But ``spline(cp)`` IS the surface -- reproducing the samples is precisely
    what the prefilter exists to do -- and the spline is what the client
    evaluates. So with the depth taken against the sample field:

        bake:    depth = water_true - sample
        client:  water = spline(cp) + depth
                       = water_true + (spline(cp) - sample)

    The bed's ripples -- its 100 mm quantisation, the meso band, the B1
    roughness -- are in ``sample`` AND reproduced in ``spline(cp)``, so they
    still CANCEL. What survives is only the spline's own reconstruction
    residual, which is a different and far smaller quantity than |cp - sample|.
    Measured bound: see ``test_water_depth_reconstruction_bound``.

    The alternative -- have the bake evaluate the normative spline itself -- was
    rejected deliberately. It buys a few tens of millimetres and costs a SECOND
    IMPLEMENTATION of the spline on the normative surface path, which is exactly
    what tilestore.h's "no second implementation exists, therefore none can
    drift" contract protects against.

    NO PREFILTER, unlike ``elevation_control_points``. The elevation plane is a
    B-spline CONTROL lattice; this is a per-pixel lookup, and a spline through a
    field with hard dry/wet boundaries would ring across the shoreline and put
    water above ground on the dry side of it.

    Returns int16 at a ``WATER_DEPTH_LSB_MM`` LSB, ``WATER_DRY_DEPTH`` for dry.

    ``level_m`` is the OPTIONAL level band (see ``WATER_NO_LEVEL``): the local
    water surface in metres absolute over DRY cells that lie near water, NaN
    everywhere else. When it is None -- the default, and what every caller did
    before the band existed -- this function is bit-identical to what it always
    was, which is the property ``test_band_leaves_the_wet_set_bit_identical``
    holds it to.

    It is an error for ``level_m`` to be finite on a WET cell. The two arrays
    come from the same producer describing the same final water, and if they
    disagree about which cells are wet then one of them is stale -- the exact
    failure this whole stage exists to avoid, since the band's own reason for
    being is that the level inside ``fill_to_local_surface`` went stale five
    stages ago.
    """
    w = np.asarray(water_m, dtype=np.float64)
    g = np.asarray(ground_m, dtype=np.float64)
    if w.shape != g.shape:
        raise ValueError(
            f"water plane {w.shape} does not match elevation {g.shape}")
    wet = np.isfinite(w)
    dry_fill = WATER_DRY_DEPTH if level_m is None else WATER_NO_LEVEL
    out = np.full(w.shape, dry_fill, np.int64)
    if wet.any():
        depth_mm = np.rint(w[wet] * 1000.0) - np.rint(g[wet] * 1000.0)
        d = np.rint(depth_mm / WATER_DEPTH_LSB_MM).astype(np.int64)
        if (d < 0).any():
            raise ValueError(
                f"{int((d < 0).sum())} wet cells quantise to a NEGATIVE depth "
                f"(min {int(d.min())} LSB); water below its own bed is not a "
                "representable state")
        if d.max() > 32767:
            raise ValueError(
                f"water depth {d.max() * WATER_DEPTH_LSB_MM / 1000.0:.1f} m "
                f"exceeds the {WATER_MAX_DEPTH_M:.1f} m the plane can store")
        out[wet] = d

    if level_m is not None:
        lv = np.asarray(level_m, dtype=np.float64)
        if lv.shape != w.shape:
            raise ValueError(
                f"level plane {lv.shape} does not match water {w.shape}")
        band = np.isfinite(lv)
        if (band & wet).any():
            raise ValueError(
                f"{int((band & wet).sum())} cells carry BOTH a depth and a "
                "level; the level band describes dry cells only, so the two "
                "arrays disagree about the final wet set")
        if band.any():
            rel_mm = np.rint(lv[band] * 1000.0) - np.rint(g[band] * 1000.0)
            v = np.rint(rel_mm / WATER_DEPTH_LSB_MM).astype(np.int64)
            # CLAMP TO -2, NEVER TO A POSITIVE. A dry cell whose level stands at
            # or above its own ground is not a contradiction -- the fill can
            # refuse a cell for want of `min_depth_m`, and the discharge budget
            # can strand one that the geometry alone would have flooded -- but a
            # POSITIVE value there would read as a DEPTH on every client and add
            # water to a cell the bake deliberately left dry. -2 says "the level
            # is 20 mm under this pixel's ground": no water at the bake's own
            # surface, and water only where the client's 10 cm ground dips below
            # it, which is precisely the sub-pixel waterline the band is for.
            v = np.clip(v, WATER_LEVEL_MIN_CP, WATER_LEVEL_MAX_CP)
            out[band] = v

    # THE ASSERTION THAT MATTERS, and it is checked on the finished plane rather
    # than on any one branch: a cell that is dry must never encode >= 0. A
    # positive value at a dry cell is not a rendering glitch, it is water added
    # to every client that reads the tile, silently and forever.
    bad = (~wet) & (out >= 0)
    if bad.any():
        raise ValueError(
            f"{int(bad.sum())} DRY cells encoded to a non-negative value "
            f"(max {int(out[bad].max())}); every client would read those as "
            "water the bake did not place")
    return out.astype(np.int16)


def water_surface_mm_from_depth(depth_cp, ground_mm):
    """The client's read, in Python: depth cp + elevation cp -> absolute mm.

    Dry reads as ``np.iinfo(np.int32).min``, mirroring voxelcore's
    ``kNoWaterMm``, so both sides answer "no water here" with the same number
    rather than with two conventions kept in step by hand.

    ``ground_mm`` is the RECONSTRUCTED SURFACE in absolute mm -- the spline
    evaluated on this tile's control lattice -- never `control_points_to_mm(cp)`
    (which is the lattice, not the surface) and never the AMPLIFIED surface.
    Adding a depth to amplified ground would make the water inherit every rill
    and bedding band the amplifier adds on top of the bake: metres of ripple on
    a surface that is flat by definition.
    """
    d = np.asarray(depth_cp)
    out = np.full(d.shape, np.iinfo(np.int32).min, np.int64)
    wet = d >= 0
    if wet.any():
        out[wet] = (np.asarray(ground_mm)[wet].astype(np.int64)
                    + d[wet].astype(np.int64) * WATER_DEPTH_LSB_MM)
    return out


def water_level_mm_from_cp(cp, ground_mm):
    """The LEVEL a cell knows about, wet or dry, in absolute mm. The band read.

    ``water_surface_mm_from_depth`` above answers "is there water standing here,
    and how high" -- the question a client asks about the BAKE's 1.875 m pixel.
    This answers "what height would water reach here if the ground let it",
    which is the question a client asks about a 10 cm VOXEL, and it is the whole
    point of the band: the caller compares this against the ground it is
    actually drawing and gets a waterline on a contour rather than on a pixel
    edge.

    Note the arithmetic is IDENTICAL in both cases -- ``ground + cp * LSB`` --
    and only the sentinels are excluded. That is not a coincidence, it is the
    encoding: a depth and a level are the same offset from the same datum, and
    the sign only says whether the bake already placed water there.

    Dry-with-no-level reads as ``np.iinfo(np.int32).min``, matching
    ``kNoWaterMm`` and this module's other reader. BOTH sentinels are honoured:
    ``WATER_DRY_DEPTH`` (-1, every tile baked before the band) and
    ``WATER_NO_LEVEL`` (-32768).
    """
    c = np.asarray(cp).astype(np.int64)
    out = np.full(c.shape, np.iinfo(np.int32).min, np.int64)
    has = (c >= 0) | ((c >= WATER_LEVEL_MIN_CP) & (c <= WATER_LEVEL_MAX_CP))
    if has.any():
        out[has] = (np.asarray(ground_mm)[has].astype(np.int64)
                    + c[has] * WATER_DEPTH_LSB_MM)
    return out


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

    THE v2 FIELDS ARE OPTIONAL IN THIS CLASS AND ONLY IN THIS CLASS: a row
    decoded from a v1 tile leaves them None, which is a fact a caller must be
    able to see ("this tile predates global identity") rather than a zero it
    would mistake for "capacity zero, floor at sea level". `pack` at v2 refuses
    a row that has none of them, so the None state can only ever arrive from a
    v1 tile, never be written into a new one.
    """

    basin_id: int
    #: Deepest cell IN THIS TILE, the client's flood-fill seed. Tile-interior
    #: pixels. For a tile-spanning basin this is not the basin's floor; that
    #: is `world_floor_px`, unpacked from `global_id`.
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

    # -- basin table v2. None on a row decoded from a v1 tile; see the class
    # docstring. All coordinates are ABSOLUTE world fine pixels.
    global_id: "int | None" = None
    #: Headroom from `surface_mm` to `spill_mm`, LITRES. See the layout note.
    capacity_l: "int | None" = None
    #: The basin's deepest cell, absolute mm. Not necessarily under `seed_px`.
    floor_mm: "int | None" = None
    #: Unclipped extent (x0, y0, x1, y1), absolute world fine pixels.
    world_bbox_px: "tuple[int, int, int, int] | None" = None
    #: The spill saddle in absolute world fine pixels -- the true one, where
    #: `outlet_px` above is clamped into this tile.
    world_outlet_px: "tuple[int, int] | None" = None
    #: BASIN_SPAN_* bits.
    span_flags: int = 0

    @property
    def has_v2(self) -> bool:
        """True when this row carries the v2 fields (identity and capacity)."""
        return self.global_id is not None

    @property
    def world_floor_px(self) -> "tuple[int, int] | None":
        """The identity anchor, unpacked from `global_id`.

        Mirrors `bake.basins.world_px_from_global_id` -- two biased 31-bit
        fields under a constant bit-62 tag. Duplicated rather than imported
        because this module must stay importable with no bake package (and no
        numba) present; `tests/test_basin_table.py` pins the two together.
        """
        if self.global_id is None:
            return None
        return (((self.global_id >> 31) & 0x7FFFFFFF) - BASIN_ID_AXIS_BIAS,
                (self.global_id & 0x7FFFFFFF) - BASIN_ID_AXIS_BIAS)

    def pack(self, version: int = BASIN_TABLE_VERSION) -> bytes:
        if version == BASIN_TABLE_VERSION_V2:
            return self._pack_v1() + self._pack_v2_tail()
        if version == BASIN_TABLE_VERSION_V1:
            return self._pack_v1()
        raise ValueError(f"unsupported basin table version {version}")

    def _pack_v1(self) -> bytes:
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

    def _pack_v2_tail(self) -> bytes:
        if not self.has_v2:
            # A v2 table with a v1-shaped row in it would ship zeros as an
            # identity -- every such basin claiming to be the lake whose floor
            # is world pixel (0, 0). Refuse instead.
            raise ValueError(
                f"basin {self.basin_id}: cannot write a v2 row without the v2 "
                "fields (global_id/capacity_l/floor_mm/world_bbox_px/"
                "world_outlet_px); it came from a v1 tile or was built by hand"
            )
        wb = self.world_bbox_px
        wo = self.world_outlet_px
        if wb is None or wo is None or self.capacity_l is None or self.floor_mm is None:
            raise ValueError(f"basin {self.basin_id}: incomplete v2 fields")
        if not (0 < int(self.global_id) <= 0xFFFFFFFFFFFFFFFF):
            raise ValueError(f"basin {self.basin_id}: global_id out of u64 range")
        if int(self.global_id) & (1 << 63):
            # voxelcore/basinledger.h tags v1 tile-local keys with bit 63 and
            # refuses a global id that sets it, so an id written with that bit
            # would be dropped by the runtime ledger while passing every
            # encoder check here. See bake.basins.BASIN_ID_TAG.
            raise ValueError(
                f"basin {self.basin_id}: global_id sets bit 63, which the "
                "runtime reserves for tile-local keys")
        if not (0 <= int(self.capacity_l) <= CAPACITY_L_MAX):
            raise ValueError(
                f"basin {self.basin_id}: capacity {self.capacity_l} litres is "
                f"outside [0, {CAPACITY_L_MAX}]"
            )
        if wb[0] > wb[2] or wb[1] > wb[3]:
            raise ValueError(f"basin {self.basin_id}: world bbox is inside out")
        if self.floor_mm > self.surface_mm:
            # The floor is under the water by definition; above it means the
            # surface and the floor came from different components.
            raise ValueError(
                f"basin {self.basin_id}: floor {self.floor_mm} mm is above its "
                f"own surface {self.surface_mm} mm"
            )
        fx, fy = self.world_floor_px  # type: ignore[misc]
        if not (wb[0] <= fx <= wb[2] and wb[1] <= fy <= wb[3]):
            # The anchor IS a cell of the component, so it is inside the
            # component's own extent. If it is not, the id and the extent were
            # measured on two different basins and the client's union rule --
            # which reads both -- would silently merge the wrong pair.
            raise ValueError(
                f"basin {self.basin_id}: the global_id anchor ({fx}, {fy}) is "
                f"outside its own world bbox {wb}"
            )
        if self.span_flags & ~BASIN_SPAN_CROSSES_TILE:
            raise ValueError(f"basin {self.basin_id}: unknown span flag bits")
        return _BASIN_ENTRY_V2_TAIL.pack(
            int(self.global_id), int(self.capacity_l), int(self.floor_mm),
            int(wb[0]), int(wb[1]), int(wb[2]), int(wb[3]),
            int(wo[0]), int(wo[1]), int(self.span_flags), _BASIN_RESERVED3,
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

        A record from a pre-v2 `basins.py` has no `global_id` attribute, so the
        v2 fields come through `getattr`: this function is called with
        hand-built record objects by tools and tests, and the shipped path must
        not depend on any of them growing a field on the same day.
        """
        gid = getattr(rec, "global_id", None)
        v2: dict = {}
        if gid:
            cap_m3 = float(getattr(rec, "capacity_m3", 0.0))
            v2 = dict(
                global_id=int(gid),
                # Rounded, not truncated -- litres are the LSB, so a truncation
                # here would bias every basin's headroom down by half a litre
                # and a ledger that fills a lake exactly would never spill.
                capacity_l=int(round(max(cap_m3, 0.0) * CAPACITY_L_PER_M3)),
                floor_mm=int(round(float(rec.floor_m) * 1000.0)),
                world_bbox_px=tuple(int(v) for v in rec.world_bbox_px),
                world_outlet_px=(int(rec.world_outlet_px[0]),
                                 int(rec.world_outlet_px[1])),
                span_flags=(0 if getattr(rec, "interior", True)
                            else BASIN_SPAN_CROSSES_TILE),
            )
        return cls(
            basin_id=int(rec.basin_id),
            seed_px=(int(rec.seed_px[0]), int(rec.seed_px[1])),
            bbox_px=tuple(int(v) for v in rec.bbox_px),  # type: ignore[arg-type]
            outlet_px=(int(rec.outlet_px[0]), int(rec.outlet_px[1])),
            spill_mm=int(round(float(rec.spill_m) * 1000.0)),
            surface_mm=int(round(float(rec.surface_m) * 1000.0)),
            kind=int(rec.kind),
            **v2,
        )

    @classmethod
    def unpack(cls, buf: bytes, offset: int = 0,
               version: int = BASIN_TABLE_VERSION_V1) -> "BasinEntry":
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
        if not (x0 <= sx <= x1 and y0 <= sy <= y1):
            # The seed is the client's flood-fill start and the bbox is the
            # bound on that fill, so a seed outside it fills nothing (or, worse,
            # something else). The C++ parser has always refused this; the
            # Python one did not, and a fixture generator got it wrong once.
            raise ValueError(f"basin {bid}: seed is outside its own bbox")
        row = cls(basin_id=bid, seed_px=(sx, sy), bbox_px=(x0, y0, x1, y1),
                  outlet_px=(ox, oy), spill_mm=spill, surface_mm=surface,
                  kind=kind)
        if version == BASIN_TABLE_VERSION_V1:
            return row
        if version != BASIN_TABLE_VERSION_V2:
            raise ValueError(f"unsupported basin table version {version}")
        (gid, cap, floor, wx0, wy0, wx1, wy1, owx, owy,
         span, reserved3) = _BASIN_ENTRY_V2_TAIL.unpack_from(
             buf, offset + BASIN_ENTRY_BYTES_V1)
        if reserved3 != _BASIN_RESERVED3:
            raise ValueError("nonzero reserved bytes in basin entry v2 tail")
        if span & ~BASIN_SPAN_CROSSES_TILE:
            raise ValueError(f"basin {bid}: unknown span flag bits 0x{span:02x}")
        if wx0 > wx1 or wy0 > wy1:
            raise ValueError(f"basin {bid}: world bbox is inside out")
        if floor > surface:
            raise ValueError(f"basin {bid}: floor is above its own surface")
        row.global_id = gid
        row.capacity_l = cap
        row.floor_mm = floor
        row.world_bbox_px = (wx0, wy0, wx1, wy1)
        row.world_outlet_px = (owx, owy)
        row.span_flags = span
        fx, fy = row.world_floor_px  # type: ignore[misc]
        if not (wx0 <= fx <= wx1 and wy0 <= fy <= wy1):
            raise ValueError(
                f"basin {bid}: the global_id anchor ({fx}, {fy}) is outside "
                f"its own world bbox {(wx0, wy0, wx1, wy1)}")
        return row


def encode_basin_table(basins: "list[BasinEntry]",
                       version: int = BASIN_TABLE_VERSION) -> bytes:
    """Serialise SECTION_BASIN_TABLE's payload.

    Ids must be 0..n-1 in order. That is not decoration: the client indexes
    the table by id, and the bake orders basins by (min_y, min_x) of extent so
    the id is a deterministic function of the surface. A gap or a repeat means
    two different tiles could disagree about which basin is "3".

    (That per-tile id is NOT the cross-tile one. Two tiles sharing a lake will
    number it differently, because each numbers only what it can see; the row's
    `global_id` is what says they are the same lake. Keeping both is why v2
    could be additive.)

    `version` exists for the fixture generators and for reproducing a v1 table
    byte for byte; the bake always writes the current version.
    """
    entry_bytes = _BASIN_ENTRY_BYTES_BY_VERSION.get(version)
    if entry_bytes is None:
        raise ValueError(f"unsupported basin table version {version}")
    for i, b in enumerate(basins):
        if b.basin_id != i:
            raise ValueError(
                f"basin ids must be 0..n-1 in order; entry {i} says {b.basin_id}"
            )
    if len(basins) > 0xFFFF:
        raise ValueError(f"{len(basins)} basins exceeds the u16 id space")
    head = _BASIN_TABLE_HEADER.pack(version, entry_bytes, len(basins))
    return head + b"".join(b.pack(version) for b in basins)


def decode_basin_table(payload: bytes) -> "list[BasinEntry]":
    """Parse SECTION_BASIN_TABLE, refusing anything it cannot read exactly.

    BOTH LAYOUTS ARE ACCEPTED -- v1 (32-byte rows) and v2 (80-byte rows) -- and
    the pairing of `table_version` with `entry_bytes` is checked rather than
    either one alone: a v2 version over 32-byte rows is not "an old table with
    a new label", it is bytes from something neither revision wrote.
    """
    if len(payload) < _BASIN_TABLE_HEADER.size:
        raise ValueError("truncated basin table header")
    version, entry_bytes, count = _BASIN_TABLE_HEADER.unpack_from(payload, 0)
    want_bytes = _BASIN_ENTRY_BYTES_BY_VERSION.get(version)
    if want_bytes is None:
        raise ValueError(f"unsupported basin table version {version}")
    if entry_bytes != want_bytes:
        raise ValueError(
            f"basin entry size {entry_bytes} != {want_bytes} for table version "
            f"{version}; these bytes were written by a different revision of "
            "the table"
        )
    want = _BASIN_TABLE_HEADER.size + count * entry_bytes
    if len(payload) != want:
        raise ValueError(
            f"basin table is {len(payload)} bytes, header says {want}"
        )
    out = [
        BasinEntry.unpack(payload, _BASIN_TABLE_HEADER.size + i * entry_bytes,
                          version)
        for i in range(count)
    ]
    for i, b in enumerate(out):
        if b.basin_id != i:
            raise ValueError(f"basin ids are not 0..n-1 (entry {i} says {b.basin_id})")
    return out


# ------------------------------------------------------------- headwaters


@dataclass
class HeadEntry:
    """One row of SECTION_HEADWATERS: where a reach starts and how much it carries.

    `px` is the tile-interior pixel and `q_m3_yr` is the discharge AT that
    cell, i.e. the faucet rate a Phase 3 emitter spawns at (m^3/yr; divide by
    31,557,600 for m^3/s).
    """

    px: tuple[int, int]
    q_m3_yr: int

    def pack(self) -> bytes:
        x, y = int(self.px[0]), int(self.px[1])
        if not (0 <= x <= 0xFFFF and 0 <= y <= 0xFFFF):
            raise ValueError(f"headwater pixel {self.px} is outside u16")
        if not (0 <= int(self.q_m3_yr) <= HEADWATER_Q_MAX):
            raise ValueError(
                f"headwater at {self.px}: Q = {self.q_m3_yr} m^3/yr is outside "
                f"[0, {HEADWATER_Q_MAX}] -- see SECTION_HEADWATERS on why this "
                "refuses rather than saturates"
            )
        return _HEADWATER_ENTRY.pack(x, y, int(self.q_m3_yr))


def encode_headwaters(heads: "list[HeadEntry]") -> bytes:
    """Serialise SECTION_HEADWATERS' payload.

    ORDERED BY (y, x), STRICTLY, and that is enforced rather than assumed. The
    producer walks a raster mask so the order falls out for free; making it a
    contract buys two things: a duplicate point (the same faucet emitted twice,
    i.e. twice the water) cannot pass, and a client may binary-search the table
    by row instead of scanning it.
    """
    if len(heads) > 0xFFFFFFFF:
        raise ValueError(f"{len(heads)} headwaters exceeds the u32 count")
    prev = (-1, -1)
    for h in heads:
        key = (int(h.px[1]), int(h.px[0]))
        if key <= prev:
            raise ValueError(
                f"headwaters must be strictly ordered by (y, x); {h.px} follows "
                f"{(prev[1], prev[0])}"
            )
        prev = key
    head = _HEADWATER_TABLE_HEADER.pack(HEADWATER_TABLE_VERSION,
                                        HEADWATER_ENTRY_BYTES, len(heads))
    return head + b"".join(h.pack() for h in heads)


def encode_headwater_arrays(xs, ys, qs) -> bytes:
    """`encode_headwaters` from three parallel arrays, without the objects.

    The bake has thousands of heads per tile as numpy arrays already; building
    a `HeadEntry` for each only to throw it away is pure allocation. Same bytes,
    same refusals -- the ordering and range checks are the vectorised form of
    the ones above, not a relaxed version of them.
    """
    x = np.asarray(xs, dtype=np.int64)
    y = np.asarray(ys, dtype=np.int64)
    q = np.asarray(qs, dtype=np.float64)
    if not (x.shape == y.shape == q.shape) or x.ndim != 1:
        raise ValueError("headwater arrays must be 1-D and the same length")
    if x.size and (x.min() < 0 or x.max() > 0xFFFF or y.min() < 0 or y.max() > 0xFFFF):
        raise ValueError("headwater pixel outside u16")
    if x.size and (np.nanmin(q) < 0.0 or np.nanmax(q) > HEADWATER_Q_MAX):
        raise ValueError(
            f"headwater Q outside [0, {HEADWATER_Q_MAX}] m^3/yr (max seen "
            f"{np.nanmax(q):.4g}) -- see SECTION_HEADWATERS on why this refuses "
            "rather than saturates"
        )
    key = y * (1 << 17) + x
    if x.size > 1 and np.any(np.diff(key) <= 0):
        raise ValueError("headwaters must be strictly ordered by (y, x)")
    rows = np.empty(x.size, dtype=_HEADWATER_DTYPE)
    rows["x"] = x
    rows["y"] = y
    # ROUNDED. Q is a float32/64 field and the wire is integer m^3/yr; a
    # truncation would bias every faucet rate down, which over a tile of heads
    # is a systematic loss of water rather than a rounding wash.
    rows["q"] = np.rint(q).astype(np.uint32)
    head = _HEADWATER_TABLE_HEADER.pack(HEADWATER_TABLE_VERSION,
                                        HEADWATER_ENTRY_BYTES, int(x.size))
    return head + rows.tobytes()


def decode_headwaters(payload: bytes) -> "list[HeadEntry]":
    """Parse SECTION_HEADWATERS, refusing anything it cannot read exactly."""
    if len(payload) < _HEADWATER_TABLE_HEADER.size:
        raise ValueError("truncated headwater table header")
    version, entry_bytes, count = _HEADWATER_TABLE_HEADER.unpack_from(payload, 0)
    if version != HEADWATER_TABLE_VERSION:
        raise ValueError(f"unsupported headwater table version {version}")
    if entry_bytes != HEADWATER_ENTRY_BYTES:
        raise ValueError(
            f"headwater entry size {entry_bytes} != {HEADWATER_ENTRY_BYTES}; "
            "these bytes were written by a different revision of the table"
        )
    want = _HEADWATER_TABLE_HEADER.size + count * entry_bytes
    if len(payload) != want:
        raise ValueError(
            f"headwater table is {len(payload)} bytes, header says {want}")
    rows = np.frombuffer(payload, dtype=_HEADWATER_DTYPE,
                         count=count, offset=_HEADWATER_TABLE_HEADER.size)
    out = [HeadEntry(px=(int(r["x"]), int(r["y"])), q_m3_yr=int(r["q"]))
           for r in rows]
    prev = (-1, -1)
    for h in out:
        key = (h.px[1], h.px[0])
        if key <= prev:
            raise ValueError(
                f"headwaters are not strictly ordered by (y, x) at {h.px}")
        prev = key
    return out


def encode_v2(
    tile: TileV2,
    *,
    raw_blocks: set[tuple[int, int]] | None = None,
    flow_raw_blocks: set[tuple[int, int]] | None = None,
    water_raw_blocks: set[tuple[int, int]] | None = None,
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
    if tile.water_cp is not None:
        flags |= FLAG_WATER_PRESENT
    if tile.heads is not None:
        flags |= FLAG_HEADS_PRESENT
    if tile.bathy_depth is not None:
        flags |= FLAG_BATHY_PRESENT

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

    if tile.water_cp is not None:
        # The SAME `_encode_plane` as elevation, at the same element width --
        # so a dry tile is 1024 CONSTANT index entries and zero data bytes, and
        # `codec` reaches it exactly as it reaches the other two planes with no
        # separate compression path to keep in step.
        water_index, water_data = _encode_plane(
            tile.water_cp.astype(np.int64),
            block_log2=tile.block_log2,
            codec=tile.codec,
            elem_dtype="<i2",
            force_raw_blocks=water_raw_blocks,
            compressor=compressor,
        )
        sections += [(SECTION_WATER_INDEX, water_index),
                     (SECTION_WATER_DATA, water_data)]

    if tile.bathy_depth is not None:
        # Same machinery again, twice. Neither plane is force-RAW: both are
        # int16, where a MED residual always ties or beats a literal, and both
        # are dominated by their saturated constant regions anyway.
        for _plane, _ix, _dx in (
            (tile.bathy_depth, SECTION_BATHY_DEPTH_INDEX, SECTION_BATHY_DEPTH_DATA),
            (tile.bathy_shore, SECTION_BATHY_SHORE_INDEX, SECTION_BATHY_SHORE_DATA),
        ):
            _index, _data = _encode_plane(
                _plane.astype(np.int64),
                block_log2=tile.block_log2,
                codec=tile.codec,
                elem_dtype="<i2",
                compressor=compressor,
            )
            sections += [(_ix, _index), (_dx, _data)]

    if tile.basins is not None:
        # LAST, and uncompressed whatever `codec` says. It is a kilobyte of
        # already-dense integers -- compressing it would buy nothing and would
        # make the one section a client must read before it can put water
        # anywhere depend on the injected decompressor.
        sections.append((SECTION_BASIN_TABLE,
                         encode_basin_table(tile.basins, tile.basin_table_version)))

    if tile.heads is not None:
        # Beside the basin table and uncompressed for the same two reasons: it
        # is dense integers, and it is read before any water can be placed.
        for h in tile.heads:
            if not (0 <= h.px[0] < tile.size and 0 <= h.px[1] < tile.size):
                raise ValueError(
                    f"headwater {h.px} is outside the {tile.size}px tile")
        sections.append((SECTION_HEADWATERS, encode_headwaters(tile.heads)))

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

    if flags & ~(FLAG_FLOW_PRESENT | FLAG_BASINS_PRESENT | FLAG_WATER_PRESENT
                 | FLAG_HEADS_PRESENT | FLAG_BATHY_PRESENT):
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

    water_cp = None
    if flags & FLAG_WATER_PRESENT:
        if SECTION_WATER_INDEX not in sections or SECTION_WATER_DATA not in sections:
            raise ValueError("water flag set but water sections are missing")
        water_cp = _decode_plane(
            sections[SECTION_WATER_INDEX], sections[SECTION_WATER_DATA],
            size=size, block_log2=block_log2, codec=codec,
            elem_dtype="<i2", out_dtype=np.int16, decompressor=decompressor,
        )
    elif SECTION_WATER_INDEX in sections or SECTION_WATER_DATA in sections:
        raise ValueError("water sections present but the water flag is clear")

    bathy_depth = bathy_shore = None
    _bathy_ids = (SECTION_BATHY_DEPTH_INDEX, SECTION_BATHY_DEPTH_DATA,
                  SECTION_BATHY_SHORE_INDEX, SECTION_BATHY_SHORE_DATA)
    if flags & FLAG_BATHY_PRESENT:
        missing = [i for i in _bathy_ids if i not in sections]
        if missing:
            raise ValueError(f"bathymetry flag set but sections {missing} are missing")
        bathy_depth, bathy_shore = (
            _decode_plane(
                sections[_ix], sections[_dx],
                size=size, block_log2=block_log2, codec=codec,
                elem_dtype="<i2", out_dtype=np.int16, decompressor=decompressor,
            )
            for _ix, _dx in ((SECTION_BATHY_DEPTH_INDEX, SECTION_BATHY_DEPTH_DATA),
                             (SECTION_BATHY_SHORE_INDEX, SECTION_BATHY_SHORE_DATA))
        )
    elif any(i in sections for i in _bathy_ids):
        raise ValueError("bathymetry sections present but the bathymetry flag is clear")

    heads = None
    if flags & FLAG_HEADS_PRESENT:
        if SECTION_HEADWATERS not in sections:
            raise ValueError("headwater flag set but SECTION_HEADWATERS is missing")
        heads = decode_headwaters(sections[SECTION_HEADWATERS])
        for h in heads:
            if not (0 <= h.px[0] < size and 0 <= h.px[1] < size):
                raise ValueError(f"headwater {h.px} is outside the tile")
    elif SECTION_HEADWATERS in sections:
        raise ValueError("SECTION_HEADWATERS present but the headwater flag is clear")

    return TileV2(
        seed=seed, x=x, y=y, size=size,
        elevation_cp=elevation_cp,
        base_offset_mm=base_offset_mm,
        quant=quant, codec=codec, bake_ver=bake_ver,
        block_log2=block_log2, flow=flow, basins=basins, water_cp=water_cp,
        heads=heads, bathy_depth=bathy_depth, bathy_shore=bathy_shore,
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
    heads: "list | tuple | np.ndarray | None" = None,
    water_surface_m: "np.ndarray | None" = None,
    water_level_m: "np.ndarray | None" = None,
    #: The bathymetry pair from `basins.bathymetry_planes`, already in wire
    #: units (int16, 10 mm depth / 100 mm signed shore distance). Passed as
    #: finished rasters rather than computed here for the same reason
    #: `basins`/`heads` are passed as rows: this module owns the FORMAT, the
    #: bake owns the HYDROLOGY, and a Euclidean transform is emphatically the
    #: latter. Both or neither -- TileV2 asserts it.
    bathy_depth: "np.ndarray | None" = None,
    bathy_shore: "np.ndarray | None" = None,
    bake_ver: int | None = None,
    codec: int = CODEC_RAW,
    block_log2: int = DEFAULT_BLOCK_LOG2,
    base_offset_mm: int | None = None,
    quant: int | None = None,
    basin_table_version: int = BASIN_TABLE_VERSION,
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
    water_cp = None
    if water_surface_m is not None:
        # AGAINST THE SAMPLE FIELD, not `cp` -- see the derivation in
        # `water_depth_control_points`. `cp` is a control lattice and stands up
        # to 5.6 m from the surface; `spline(cp)`, which is what the client
        # evaluates, does not. The level band takes the same datum for the same
        # reason: it is the same offset from the same ground.
        water_cp = water_depth_control_points(
            water_surface_m, elevation_m, base_offset_mm=base_offset_mm,
            quant=quant, level_m=water_level_m
        )
    elif water_level_m is not None:
        raise ValueError(
            "water_level_m was given without water_surface_m; a level band "
            "with no water plane to carry it would be silently dropped")

    basin_rows = None
    if basins is not None:
        # A tuple() of zero basins is NOT None: a tile that was surveyed and
        # holds nothing must say so, or a client cannot tell it apart from a
        # tile baked before the registry existed.
        basin_rows = [
            b if isinstance(b, BasinEntry) else BasinEntry.from_record(b)
            for b in basins
        ]
    head_rows = None
    if heads is not None:
        # A tuple() of zero heads is NOT None, exactly as for `basins`.
        # Accepts either HeadEntry rows or the bake's own (N, 3) integer array
        # of (x, y, q) -- the latter is what `BakeResult.water_heads` is, and
        # converting it here keeps the array form out of the wire code.
        head_rows = [
            h if isinstance(h, HeadEntry)
            else HeadEntry(px=(int(h[0]), int(h[1])), q_m3_yr=int(h[2]))
            for h in heads
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
            heads=head_rows,
            water_cp=water_cp,
            bathy_depth=(None if bathy_depth is None
                         else np.ascontiguousarray(bathy_depth, np.int16)),
            bathy_shore=(None if bathy_shore is None
                         else np.ascontiguousarray(bathy_shore, np.int16)),
            basin_table_version=int(basin_table_version),
        )
    )
