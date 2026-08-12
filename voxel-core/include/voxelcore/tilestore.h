#pragma once
// Decoded tile storage + sampling (plan §3.1 step 2, §3.4 ITerrainSource).
// TileData parses the exact wire format produced by
// terrain-service/terrain_service/tile_codec.py (encode/decode); TileGridSampler
// serves a grid of decoded tiles for one (seed, scale) through ITileSampler,
// the same interface amplifier.h consumes for both local and remote
// ITerrainSource implementations.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "voxelcore/tiles.h"

namespace vxc {

// Decoded tile. Wire format (all little-endian, matches tile_codec.py
// exactly):
//   magic    4B  "VXTL"
//   version  u16 (1)
//   seed     u64
//   x, y     i32  tile coords (tile (0,0) covers pixels [0,512) each axis)
//   scale    u8   (1 => 30m/px, 8 => 3.75m/px -- see tilePixelSizeMm below)
//   size     u16  (512)
//   elevation int16[size*size], row-major, y outer, metres
//   climate   uint8[4][size*size]  (temperature, seasonality, precipitation,
//                                   precipVariability planes)
struct TileData {
    static constexpr uint32_t kTileSize = 512;
    static constexpr uint32_t kPixelCount = kTileSize * kTileSize;
    static constexpr uint32_t kClimateChannels = 4;
    static constexpr uint16_t kFormatVersion = 1;

    uint64_t seed = 0;
    int32_t x = 0, y = 0;
    uint8_t scale = 1;
    std::vector<int16_t> elevation;                             // kPixelCount, metres
    std::array<std::vector<uint8_t>, kClimateChannels> climate;  // each kPixelCount

    int16_t elevationAt(uint32_t px, uint32_t py) const {
        return elevation[py * kTileSize + px];
    }
    uint8_t climateAt(uint32_t channel, uint32_t px, uint32_t py) const {
        return climate[channel][py * kTileSize + px];
    }

    // Exact parse: validates magic, version, declared size, AND total byte
    // length (rejects both truncated input and trailing bytes) — the same
    // rejects tile_codec.decode() enforces. Returns nullopt on any mismatch.
    static std::optional<TileData> parse(const uint8_t* data, size_t size);
};

// mm-per-pixel by scale; must match terrain-service's
// tile_codec.PIXEL_SIZE_MM and ITileSampler::pixelSizeMm's contract. Returns
// 0 for unsupported scales.
//
// scale is a SUPERSAMPLE factor on the pinned 30 m checkpoint, so scale 8 is
// 30 m / 8 = 3.75 m/px = 3750 mm. The old 11250 was 90 m / 8, left over from
// the superseded 90 m model -- wrong by 3x. tile_codec.py and tiles.h were
// corrected when that was found; THIS copy was missed, in the one file
// tile_codec.py's own mirror comment names. No scale-8 tile has ever been
// generated, so no cached tile is affected and no golden moves.
//
// scale 16 is the .vxtl v2 baked fine tier: 30 m / 16 = 1.875 m/px = 1875 mm,
// i.e. an 8192x8192 grid over the same 15.36 km footprint as one s1 tile
// (docs/vxtl-v2-format.md §1). MIRROR: tile_codec.py:43 and tiles.h.
constexpr int32_t tilePixelSizeMm(uint8_t scale) {
    return scale == 1 ? 30000 : (scale == 8 ? 3750 : (scale == 16 ? 1875 : 0));
}

// Reads just the `version` field of a .vxtl buffer, so callers can route bytes
// to TileData::parse (v1) or FineTile::parse (v2) without try-both. Validates
// the magic; returns nullopt if the buffer is too short or not a .vxtl.
std::optional<uint16_t> vxtlVersion(const uint8_t* data, size_t size);

// One row of the v2 section table (§3).
struct FineSectionEntry {
    uint32_t id = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
};

// Everything a RANGE FETCHER needs to decide what to ask for next, read out of
// the head of the file: which sections exist, where they are, how long the file
// is, and which planes the flags declare.
//
// DELIBERATELY NOT A VALIDATOR, and the distinction matters. This is the same
// peek `vxtlVersion` is: it checks only enough to trust the offsets it hands
// back (magic, version, a table that fits in the probe). The authority on
// whether a tile is well-formed remains FineTile::parsePartial, which every
// fetched tile still goes through and which re-reads all of this from the
// bytes. So a header this function accepts can still be rejected there -- that
// is the intended order, not a gap: planning a fetch and trusting a tile are
// different decisions and only the second one is safety-critical.
//
// Returns false if `head` is not a v2 .vxtl or is too short to hold the whole
// section table -- in which case the caller must re-probe with a larger head.
// `fileSize` comes back as the end of the last section, which is what the
// format's own "nothing past the last section" rule makes authoritative.
bool readFineSectionTable(const uint8_t* head, size_t headLen, uint16_t& flags,
                          uint64_t& fileSize, std::vector<FineSectionEntry>& out);

// ---------------------------------------------------------------------------
// .vxtl v2 -- the baked fine tier. docs/vxtl-v2-format.md is the FROZEN
// CONTRACT this implements; the Python encoder
// (terrain-service/terrain_service/tile_codec.py) is built independently
// against the same document, so every deviation here is a corrupt world rather
// than a build error. Section references below are to that document.
//
// WHAT THE PLANE HOLDS (§2, and it is the whole contract): absolute, already
// prefiltered uniform cubic B-spline CONTROL POINTS -- not samples, not
// residuals against the coarse tier. int16, LSB = `quant` (100 or 250 mm),
// about the per-tile datum `base_offset_mm`:
//
//     elevation_mm(i, j) = base_offset_mm + int32(cp[i][j]) * quant_mm
//
// The client never interpolates samples; it evaluates the spline (§8, which is
// bit-for-bit the carrier already shipped in amplifier.cpp) directly on this
// lattice. That is why FineTileSampler is an ITileSampler returning LATTICE
// VALUES from elevationMm(): the amplifier's carrier consumes exactly this and
// nothing new has to be written to evaluate the spline.
//
// DECODE IS A PURE INTEGER FUNCTION OF THE BYTES (§7) -- these bytes are the
// multiplayer authority. No floating point, and every division truncates
// (plain C++ `/`), never floors.
// ---------------------------------------------------------------------------

// Production fine grid edge in pixels (§3 `size`). 8192 * 1875 mm == 15.36 km
// == the footprint of one 512 px s1 tile at 30 m/px. `size` is a header field,
// not a constant: conformance fixtures ship smaller edges so they can be
// committed, and FineTile validates the field structurally against this as the
// maximum rather than pinning it. FineTileSampler addresses the grid by the
// loaded tiles' own size and refuses to mix two different ones.
inline constexpr uint32_t kFineTileSize = 8192;
inline constexpr uint8_t kFineTileScale = 16;
inline constexpr uint16_t kFineFormatVersion = 2;

// Byte offset of the section table, i.e. the size of the fixed header (§3).
// The first 25 bytes are positionally identical to v1 by construction, so a v1
// parser fails on `version` rather than on garbage.
inline constexpr size_t kFineHeaderBytes = 43;
// One section-table entry: u32 id + u64 offset + u64 length.
inline constexpr size_t kFineSectionEntryBytes = 20;
// One block-index entry: u64 offset + u32 comp_len + u8 mode + i16 const_cp +
// u8 resid_bits + u8[4] pad (§4).
inline constexpr size_t kFineBlockEntryBytes = 20;

enum FineSectionId : uint32_t {
    kSectionElevIndex = 1,
    kSectionElevData = 2,
    kSectionFlowIndex = 3,
    kSectionFlowData = 4,
    // The per-tile basin registry (docs/watershed-system-plan.md P1, bake_ver
    // 8). A flat table of tens of rows, never a plane -- the bake decides
    // where the lakes are and how high they stand, and this is the whole of
    // what the client needs to put water in them.
    kSectionBasinTable = 5,
    // The graded water-surface plane (watershed plan P2, bake_ver 9). Same
    // block machinery, element width and DATUM as elevation -- int16 control
    // points relative to `baseOffsetMm` at `quantMm()` -- with
    // `kWaterDryDepth` where there is no water.
    //
    // Sharing the elevation datum is what makes the query cheap AND correct: a
    // client asks "is there water over this column, and how deep" with two
    // reads on one datum and a subtraction, instead of converting between two
    // coordinate spaces at every voxel of a brick sweep.
    kSectionWaterIndex = 6,
    kSectionWaterData = 7,
    // Headwater points (water re-architecture Phase 1, bake_ver 24): the cells
    // where drawn water STARTS and the discharge at each. A flat table like the
    // basin registry -- a head is a point, not a plane -- and it is the faucet
    // list the fluid solver spawns from. The bake has computed this mask since
    // bake_ver 9 and discarded it unused on every tile until now.
    kSectionHeadwaters = 8,
    // The bathymetry pair (water appearance plan, bake_ver 27): per-cell lake
    // DEPTH and SIGNED DISTANCE TO SHORE. Two more planes through the same
    // block machinery, element width and index layout as elevation and water --
    // int16 per fine pixel, block_log2 8 -- because both are consumed per-pixel
    // by the water material and neither can be derived from a basin row.
    //
    // WHY THE CLIENT CANNOT JUST COMPUTE THEM (tile_codec.py states this at
    // length and it is the reason these exist at all): a lake's FOOTPRINT is a
    // flood fill over data the client already holds, which is why
    // lakes.h re-derives it. Depth needs the datum minus ground at every cell,
    // and shore distance needs a Euclidean transform over a whole basin -- a
    // global operation over up to 2.5 million cells that does not decompose per
    // column. The bake does it once; the client fetches a block.
    //
    // WHY TWO PLANES AND NOT ONE INTERLEAVED ONE: the MED predictor runs along
    // a scanline, and the two fields have unrelated gradients -- interleaving
    // them hands the predictor a sawtooth and costs more than a second index.
    kSectionBathyDepthIndex = 9,
    kSectionBathyDepthData = 10,
    kSectionBathyShoreIndex = 11,
    kSectionBathyShoreData = 12,
};

// SECTION_BASIN_TABLE payload layout, mirroring terrain_service/tile_codec.py.
//
//   u16 table_version | u16 entry_bytes | u32 count | count * entry_bytes
//
// `entry_bytes` is redundant with the section length, and deliberately so: a
// decoder that disagrees with the encoder about the row size gets a mismatch
// it can refuse, instead of reading 33-byte records out of a 32-byte stream
// and producing plausible garbage.
//
// VERSION 2 (bake_ver 24) APPENDS 48 bytes and changes nothing in front of
// them, so bytes 0..31 of a v2 row are a whole v1 row positionally. BOTH ARE
// ACCEPTED: v1 tiles are on disk in shipped namespaces today, and a reader
// that refused them would strand every world baked before this one.
inline constexpr uint16_t kBasinTableVersionV1 = 1;
inline constexpr uint16_t kBasinTableVersionV2 = 2;
// The newest layout this build WRITES about / expects from a fresh bake. Kept
// as the old name because it is quoted in the fixture tools.
inline constexpr uint16_t kBasinTableVersion = kBasinTableVersionV2;
inline constexpr size_t kBasinTableHeaderBytes = 8;
inline constexpr size_t kBasinEntryBytes = 32;
inline constexpr size_t kBasinEntryBytesV2 = 80;
// `spanFlags` bit0: this row's extent leaves this tile, i.e. expect a partner
// row in a neighbour describing the same lake.
inline constexpr uint8_t kBasinSpanCrossesTile = 0x1;

// `globalId`'s coordinate packing, mirroring bake/basins.py. Each axis is
// stored as `world_px + kBasinIdAxisBias` in 31 bits, under a constant bit-62
// tag -- which keeps bit 63 clear and the id non-zero, both required by
// basinledger.h's BasinId (it tags v1 tile-local keys with bit 63 and reserves
// 0 for "not a basin"). The bias spans +/-1.07e9 px = +/-2.01 million km at
// 1.875 m/px; a coordinate past it is refused at bake time rather than folded.
inline constexpr int64_t kBasinIdAxisBias = int64_t(1) << 30;
inline constexpr uint64_t kBasinIdTag = uint64_t(1) << 62;

// What kind of place a registered depression is (bake/basins.py's KIND_*).
// The ORDER is load-bearing: >= kBasinLakeTerminal means "holds standing
// water", which is the only test most callers need.
enum BasinKind : uint8_t {
    kBasinDryPlaya = 0,
    kBasinSaltFlat = 1,
    kBasinSeasonal = 2,
    kBasinLakeTerminal = 3,
    kBasinLakeOverflowing = 4,
    kBasinKindCount = 5,
};

// One registered basin. 32 bytes on the wire and in memory -- this struct is
// filled field by field from the byte reader, never memcpy'd over the file,
// so no packing pragma is needed and none is relied on.
//
// Pixel coordinates are TILE-LOCAL, in the same space as the elevation plane,
// so a client indexes them with no transform. Elevations are ABSOLUTE
// millimetres, NOT relative to baseOffsetMm: a basin is read by gameplay code
// that never touches the control-point datum, and sharing one would couple a
// water query to the elevation codec's internals.
struct BasinEntry {
    uint16_t basinId = 0;
    //: Deepest cell -- the client's flood-fill seed.
    uint16_t seedX = 0, seedY = 0;
    //: Inclusive extent. The flood fill is bounded by this, which is what
    //: makes the per-basin cost O(bbox) instead of O(tile).
    uint16_t bboxX0 = 0, bboxY0 = 0, bboxX1 = 0, bboxY1 = 0;
    //: Spill cell -- the head of the outlet channel.
    uint16_t outletX = 0, outletY = 0;
    //: Fill level on the FINAL surface, and the equilibrium water surface.
    //: surfaceMm <= spillMm always; they are equal when the basin overflows,
    //: and surfaceMm sits at the floor when it is dry.
    int32_t spillMm = 0;
    int32_t surfaceMm = 0;
    uint8_t kind = kBasinDryPlaya;

    // --- basin table v2 (bake_ver 24) ------------------------------------
    //
    // Which layout this row came from. 1 means the v2 fields below are
    // ABSENT, not zero, and a consumer must branch on it rather than read a
    // zero identity: every v1 row would otherwise claim to be the same lake
    // (the one whose floor is world pixel 0,0) and a union by id would merge
    // the entire world into it.
    uint16_t tableVersion = kBasinTableVersionV1;
    bool hasV2() const { return tableVersion >= kBasinTableVersionV2; }

    //: CROSS-TILE IDENTITY: the basin's floor cell in absolute world fine
    //: pixels, packed as kBasinIdTag | ((x + BIAS) << 31) | (y + BIAS). Two
    //: tiles that both register one physical basin write the same value with
    //: no cross-tile communication at bake time. A packing, not a hash -- so
    //: there is no collision argument to make, and `globalIdWorldX/Y` read it
    //: back as a place you can go and look at.
    //:
    //: BIASED 31-BIT FIELDS UNDER A CONSTANT BIT-62 TAG, not two's-complement
    //: u32 halves, and the reason is basinledger.h: that header reserves bit
    //: 63 for "this key is a v1 tile-local id" and 0 for "not a basin". A
    //: negative world x sets bit 63 under the naive packing, and the wet
    //: alpine block is entirely at negative tiles -- so every basin in the
    //: world we actually look at would have been refused by the ledger.
    //:
    //: ITS LIMIT, stated where it is defined: the two tiles agree only where
    //: their padded surfaces agree over the overlap, which the apron makes
    //: almost always true and does not guarantee. A client must therefore
    //: treat matching ids as the FAST PATH of its union and fall back to
    //: "world bboxes overlap and spillMm agree" -- both fields are here for
    //: exactly that reason.
    uint64_t globalId = 0;
    //: Headroom between surfaceMm and spillMm, in LITRES. u64 because a real
    //: lake overflows u32 immediately: 566 ha (the wet block's largest kept
    //: coverage) at 0.76 m already reaches 4.295e9 litres.
    uint64_t capacityLitres = 0;
    //: The deepest cell's elevation, absolute mm. NOT necessarily the ground
    //: under seedX/seedY: for a spanning basin the floor can be in the
    //: neighbour. floorMm <= surfaceMm <= spillMm.
    int32_t floorMm = 0;
    //: The component's UNCLIPPED extent in absolute world fine pixels. The
    //: u16 bbox above is clipped to this tile so a client can index its own
    //: plane; this is the whole basin, and it is what the union rule compares.
    int32_t worldX0 = 0, worldY0 = 0, worldX1 = 0, worldY1 = 0;
    //: The spill saddle in absolute world fine pixels -- the true one, where
    //: outletX/outletY are clamped into this tile and are a lie for a basin
    //: that spills outside it. A spillway faucet must use this.
    int32_t worldOutletX = 0, worldOutletY = 0;
    //: kBasinSpan* bits.
    uint8_t spanFlags = 0;

    //: True when this basin's extent leaves this tile -- i.e. a partner row
    //: exists in a neighbour and the two describe one lake.
    bool crossesTile() const { return (spanFlags & kBasinSpanCrossesTile) != 0; }
    int32_t globalIdWorldX() const {
        return static_cast<int32_t>(static_cast<int64_t>((globalId >> 31) & 0x7FFFFFFFull) -
                                    kBasinIdAxisBias);
    }
    int32_t globalIdWorldY() const {
        return static_cast<int32_t>(static_cast<int64_t>(globalId & 0x7FFFFFFFull) -
                                    kBasinIdAxisBias);
    }

    //: True when this basin holds standing water at equilibrium.
    bool holdsWater() const { return kind >= kBasinLakeTerminal; }
};

// SECTION_HEADWATERS payload layout, mirroring terrain_service/tile_codec.py.
//
//   u16 table_version | u16 entry_bytes | u32 count | count * 8 bytes
//   row: u16 px | u16 py | u32 q_m3_yr
//
// Rows are STRICTLY ordered by (y, x), which is both free (the bake walks a
// raster mask) and load-bearing: a duplicate point is the same faucet emitted
// twice, i.e. twice the water at one place.
inline constexpr uint16_t kHeadwaterTableVersion = 1;
inline constexpr size_t kHeadwaterTableHeaderBytes = 8;
inline constexpr size_t kHeadwaterEntryBytes = 8;

// One headwater: where a reach starts, and what it carries.
struct HeadEntry {
    //: Tile-local pixel, same space as the elevation plane.
    uint16_t px = 0, py = 0;
    //: Discharge AT that cell, m^3/yr -- the faucet rate. Divide by 31'557'600
    //: for m^3/s. u32 holds the world's observed maximum (2.3e8) 18x over; the
    //: ENCODER refuses anything larger rather than saturating, because a
    //: saturated rate is a plausible number that silently understates a river.
    uint32_t qM3PerYear = 0;
};

enum FineBlockMode : uint8_t {
    kBlockConstant = 0,
    kBlockCoded = 1,
    kBlockRaw = 2,
};

enum FineCodec : uint8_t {
    kCodecRaw = 0,
    kCodecZstd = 1,
};

// §3 `predictor`: 1 == the LOCO-I / JPEG-LS median edge predictor of §5.
inline constexpr uint8_t kPredMed = 1;

// §3 `flags`. Only bit0 is defined; any other bit set is rejected, because an
// undefined flag can only mean the bytes were produced by something this
// decoder does not understand.
inline constexpr uint16_t kFineFlagFlowPresent = 0x1;
// §3 `flags` bit1 (bake_ver 8): SECTION_BASIN_TABLE is present. Set even when
// the table has ZERO rows -- "this tile was surveyed and holds nothing" and
// "this tile predates the registry" are different facts and a client that
// conflated them would put no water in a world that has some.
inline constexpr uint16_t kFineFlagBasinsPresent = 0x2;
// §3 `flags` bit2 (bake_ver 9): SECTION_WATER_* is present. Set even when the
// plane is entirely dry, for the same reason as the basin flag above: "this
// tile was surveyed and carries no rivers" and "this tile predates the water
// plane" are different facts, and a client that conflated them would draw no
// rivers in a world that has them and never be able to tell why.
inline constexpr uint16_t kFineFlagWaterPresent = 0x4;
// §3 `flags` bit3 (bake_ver 24): SECTION_HEADWATERS is present. Set even when
// the tile has no heads, for the third time and the same reason: "surveyed, no
// reach starts here" and "baked before headwaters existed" are different facts.
inline constexpr uint16_t kFineFlagHeadsPresent = 0x8;
// §3 `flags` bit4 (bake_ver 27): SECTION_BATHY_* are present. Set even on a
// tile with no lakes, for the fourth time and the same reason: "surveyed, no
// basins here" and "baked before bathymetry existed" are different facts, and
// only the flag tells them apart. ONE BIT COVERS BOTH PLANES -- they are
// computed together from one extent pass, and there is no case where a tile
// carries depth without shore distance or the reverse, so the parser below
// requires all four sections or none. Mirrors tile_codec.FLAG_BATHY_PRESENT.
inline constexpr uint16_t kFineFlagBathyPresent = 0x10;
// EVERY BIT THIS BUILD IMPLEMENTS. `flags & ~kFineFlagsKnown` is the parser's
// hard refusal (kUnknownFeature), so a bit missing from this mask means every
// tile carrying it is thrown away whole -- which is the correct behaviour for a
// reader that is genuinely too old, and a self-inflicted outage for one that
// merely forgot to list a bit it does implement.
inline constexpr uint16_t kFineFlagsKnown = kFineFlagFlowPresent | kFineFlagBasinsPresent |
                                            kFineFlagWaterPresent | kFineFlagHeadsPresent |
                                            kFineFlagBathyPresent;

// The newest `bake_ver` whose ON-TILE FEATURES this build implements -- i.e.
// the last bake_ver that added a section id or a flag bit that the parser
// above now understands. bake_ver 9 introduced SECTION_WATER_* and
// kFineFlagWaterPresent; 10-12 are product bumps over the same layout, so
// this build reads everything up to and including 12.
//
// REPORTING ONLY. Nothing rejects a tile on this number and nothing may start
// to: the format's compatibility rule is the FLAGS and the section table (an
// unknown section id is ignored; an unknown flag bit is refused), and a
// second, redundant version gate would refuse tiles that are perfectly
// readable the moment the counter is bumped for a reason that never touched
// the wire. What it is for is the one sentence a refusal has to be able to
// say: "this file is bake_ver 12 and this build understands up to N". Without
// both numbers, a stale binary and a corrupt file produce the same message,
// which is exactly how one stale DLL cost an evening.
//
// Bump this in the same commit that teaches the parser a new flag bit or
// section id, never on its own.
//
// 12 -> 24 at basin table v2: this commit teaches the parser BOTH a new
// section id (kSectionHeadwaters) and a new flag bit (kFineFlagHeadsPresent),
// which is exactly the rule above. 13-23 were product bumps over an unchanged
// layout, which is why the counter lagged twelve versions behind the bake and
// was right to.
//
// 24 -> 27 at the bathymetry pair: this commit teaches the parser four new
// section ids (kSectionBathy*) and a new flag bit (kFineFlagBathyPresent), so
// the same rule applies again. 25 and 26 were product bumps over an unchanged
// layout.
inline constexpr uint16_t kFineMaxKnownBakeVer = 27;

// The dry sentinel in the water plane, in CONTROL-POINT units. INT16_MIN, the
// one value the elevation codec's own range can never legitimately carry as a
// water surface -- see terrain_service/tile_codec.py's WATER_DRY_CP, which
// this mirrors. A decoder must test for it BEFORE applying
// `elevationMmFromCp`, or the sentinel becomes an ordinary (very low)
// elevation and every dry pixel reads as water 3.2 km underground.
inline constexpr int16_t kWaterDryDepth = -1;

//: The other "no level here" sentinel, emitted by encoders that ship the level
//: band. Distinct from kWaterDryDepth only so a tile can say "surveyed for
//: levels, none at this cell" as against "predates levels"; both decode to
//: kNoWaterMm. Mirrors tile_codec.WATER_NO_LEVEL.
inline constexpr int16_t kWaterNoLevel = -32768;

//: How far BELOW its own ground a dry pixel's level may be and still be worth
//: shipping. Mirrors tile_codec.WATER_LEVEL_BAND_MM, and it is derived rather
//: than tuned: amplifier.cpp static_asserts fine-tier |amplified - carrier| <=
//: 2280 mm, so a pixel standing further than that above its level cannot hold a
//: single 10 cm voxel below it.
inline constexpr int32_t kWaterLevelBandMm = 2400;

// Millimetres per stored water-depth LSB. Mirrors tile_codec.WATER_DEPTH_LSB_MM.
inline constexpr int32_t kWaterDepthLsbMm = 10;

// "No baked water over this column." INT32_MIN rather than a sentinel like
// kSeaLevelMm because a lake CAN sit below sea level datum-wise in principle
// and because `CavernColumn::floodZMm` already uses exactly this value for
// exactly this meaning -- one convention, several producers.
//
// DEFINED HERE, not in lakes.h where it began: the water PLANE is decoded at
// this level and has to answer "dry" in the same currency the lake sampler
// answers it in, and lakes.h includes this header rather than the other way
// round. Two constants with one meaning is how the three discharge currencies
// happened; see bake/water.py.
inline constexpr int32_t kNoWaterMm = INT32_MIN;

// --- SECTION_BATHY_* wire units (bake_ver 27) -------------------------------
//
// EVERY ONE OF THESE MIRRORS A NAMED CONSTANT IN
// terrain_service/bake/basins.py -- BATHY_DEPTH_LSB_MM, BATHY_SHORE_LSB_MM,
// BATHY_SHORE_CLAMP_M and BATHY_DRY_DEPTH -- and the names are kept parallel on
// purpose so a grep for either side finds the other. The producer is the
// authority; if these two lists ever disagree the tiles are right and this
// header is wrong.
//
// The conversions below exist so that NO CONSUMER RE-DERIVES AN LSB. A shader
// or a gameplay query that multiplies by its own 10 or 100 is a second copy of
// the format, and the failure mode is not a crash: it is a lake that is ten
// times too deep, or a foam band ten times too wide, in a build nobody thinks
// touched the codec.

//: Millimetres per stored DEPTH unit. Mirrors basins.BATHY_DEPTH_LSB_MM.
inline constexpr int32_t kBathyDepthLsbMm = 10;
//: Millimetres per stored SHORE-DISTANCE unit. Mirrors
//: basins.BATHY_SHORE_LSB_MM -- the elevation plane's own 100 mm.
inline constexpr int32_t kBathyShoreLsbMm = 100;

//: "Not inside any basin extent", i.e. this cell is not lake bed. -1, the same
//: marker SECTION_WATER_* uses for dry (kWaterDryDepth), so a consumer that
//: knows one plane knows both -- basins.BATHY_DRY_DEPTH says exactly that.
//:
//: TEST IT AS `< 0`, NOT `== -1`, which is what bathyDepthIsDry does: a depth
//: is non-negative by construction (water stands above its own bed), so the
//: whole negative half of the range is available to mean "no water" and no
//: future sentinel can turn a dry cell into a 300 m lake.
inline constexpr int16_t kBathyDryDepth = -1;

//: Where the signed shore distance saturates, in stored units: +/-1000, i.e.
//: +/-100 m. Mirrors basins.BATHY_SHORE_CLAMP_M (100.0 m) divided by the LSB.
//: The clamp is what makes the plane nearly free -- every block further than
//: this from any water is one repeated value and encodes MODE_CONSTANT at zero
//: data bytes.
inline constexpr int16_t kBathyShoreClampUnits = 1000;
inline constexpr int32_t kBathyShoreClampMm = kBathyShoreClampUnits * kBathyShoreLsbMm;

//: "This cell has no lake depth." Returned by bathyDepthMm for the dry
//: sentinel, and INT32_MIN for the same reason kNoWaterMm is: it is the one
//: value a millimetre depth can never legitimately be, so it cannot be summed
//: or interpolated into a plausible number by accident.
inline constexpr int32_t kNoBathyDepthMm = INT32_MIN;

//: Is this cell outside every basin extent?
constexpr bool bathyDepthIsDry(int16_t depthUnits) { return depthUnits < 0; }

//: Lake depth in millimetres, or kNoBathyDepthMm for a dry cell. The sentinel
//: test lives HERE rather than at each call site precisely so it cannot be
//: forgotten at one of them -- the same argument waterMmFromDepth makes.
//:
//: A stored 0 is WET, at exactly the bed: the extent's outermost ring, where
//: the datum meets the ground, quantises there. `<= 0 means dry` is the bug
//: this function exists to make impossible, and the fixture carries that cell.
constexpr int32_t bathyDepthMm(int16_t depthUnits) {
    return bathyDepthIsDry(depthUnits) ? kNoBathyDepthMm
                                       : static_cast<int32_t>(depthUnits) * kBathyDepthLsbMm;
}

//: SIGNED distance to the nearest shoreline in millimetres: POSITIVE inside
//: water, NEGATIVE on land, saturating at +/-kBathyShoreClampMm. No sentinel --
//: every value is meaningful, including the saturated ones, which mean "at
//: least 100 m from any shore on this side of it".
constexpr int32_t bathyShoreMm(int16_t shoreUnits) {
    return static_cast<int32_t>(shoreUnits) * kBathyShoreLsbMm;
}

//: Which side of the shoreline this cell is on. The SIGN is the whole point of
//: the plane (basins.py: "the sign is load-bearing, not a convenience") -- land
//: within a few decimetres of a lake is exactly the set that should be darkened
//: as wet, and nothing else in the format can express it.
//:
//: Zero reads as LAND. It is not reachable from the bake -- the distance
//: transform measures to the nearest cell of the opposite class, so the nearest
//: any cell gets to the shoreline is one pixel, 1.875 m == 19 stored units --
//: but a rule that leaves the boundary undefined is a rule two consumers will
//: split on.
constexpr bool bathyShoreIsWater(int16_t shoreUnits) { return shoreUnits > 0; }

// True for codec values the FORMAT defines. Says nothing about whether this
// process can decode them -- see fineCodecNeedsDecompressor below.
constexpr bool fineCodecKnown(uint8_t codec) {
    return codec == kCodecRaw || codec == kCodecZstd;
}
// True for codecs whose block payloads are compressed frames and therefore
// need an injected decompressor (below) to read.
constexpr bool fineCodecNeedsDecompressor(uint8_t codec) { return codec == kCodecZstd; }

// ---------------------------------------------------------------------------
// CODEC_ZSTD: the decompressor is INJECTED, never linked into voxel-core.
// docs/vxtl-v2-format.md §3 decides this, and the second reason is the serious
// one: voxel-core has zero third-party dependencies on purpose, AND it is
// compiled into a static library linked into a UE binary. A zstd vendored in
// here would be a SECOND copy of zstd's symbols in that binary the moment the
// engine or any plugin brings its own -- an ODR/symbol-collision hazard whose
// failure mode is wrong terrain, not a link error.
//
// (§3 justifies that by saying UE 5.8 ships zstd in Engine/Source/ThirdParty.
// Checked 2026-07-29 against the installed UE 5.8: the binary/launcher
// distribution does NOT -- it ships Oodle and LZ4. It DOES carry a zstd
// statically linked inside ThirdParty/Blosc's libblosc.lib, which is the
// collision the argument was about, just not the borrowable copy §3 expected.
// See ue-project/Source/VoxelEarth/VoxelTileCodec.h for the full finding. The
// conclusion is unchanged and in fact firmer: WHICH zstd a binary uses is the
// host's decision, so voxel-core stays out of it entirely.)
//
// The UE module hands one over (VoxelTileCodec.h), a headless harness hands
// over its own, and voxel-core builds and tests standalone with no compression
// library present at all.
//
// Decode stays a pure integer function of the bytes (§7) because zstd frame
// decode is bit-exact by format definition. voxel-core cannot enforce that of
// an arbitrary injected callback, so it enforces the part it can: the frame
// must expand to EXACTLY the length the header implies, and nothing on this
// side of the boundary is allowed to vary with the host.
// ---------------------------------------------------------------------------

// Decompress exactly one §4 block frame. Every clause here is load-bearing:
//
//   * `src`/`srcLen` is that block's stored frame and nothing else -- blocks
//     are independent (§4), so there is no dictionary, no shared context and
//     no cross-block state to thread through. A callback that keeps state
//     between calls breaks the format's random-access guarantee.
//   * `dst`/`dstLen` is the caller's buffer. voxel-core always knows the exact
//     decompressed size from the header (block pixels x the mode's element
//     width), so the callback never allocates and never reports a size.
//   * Return true ONLY if exactly `dstLen` bytes were produced. A frame that
//     expands to fewer or more bytes -- truncated, padded, or simply not the
//     frame the index claims -- MUST return false. This is the check that
//     stops a corrupt frame becoming plausible terrain, and it is the only
//     length check that means anything under CODEC_ZSTD: `comp_len` is the
//     COMPRESSED length there and constrains nothing about the contents.
//   * Must not throw. voxel-core is exception-free on the query path.
//   * Must be safe to call from several threads at once. Statelessness gets
//     that for free; a shared decompression context would not, and would also
//     break the clause above it. FineTileSampler::prewarm exists precisely so
//     a region can be made resident from one thread and then read from many.
//
// `user` is the opaque context handed over at registration. A plain function
// pointer rather than a std::function on purpose: trivially copyable, no
// allocation, and it survives the C-ABI-ish boundary into a UE module cleanly.
using FineDecompressFn = bool (*)(void* user, const uint8_t* src, size_t srcLen, uint8_t* dst,
                                  size_t dstLen);

struct FineDecompressor {
    FineDecompressFn fn = nullptr;
    void* user = nullptr;

    bool valid() const { return fn != nullptr; }
    bool operator()(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) const {
        return fn != nullptr && fn(user, src, srcLen, dst, dstLen);
    }
};

// Why a reason code rather than a bare nullopt: a tile that declares
// CODEC_ZSTD with no decompressor injected is a DEPLOYMENT mistake (the host
// forgot to register one), while a tile that fails its block index is corrupt
// data. Both must refuse the tile, but a caller that cannot tell them apart
// logs the wrong thing and chases the wrong bug -- and the failure mode this
// whole path exists to prevent, silently decoding a compressed frame as
// literal bytes, is a desync rather than a glitch.
enum class FineError : uint8_t {
    kNone = 0,
    kFileUnreadable,   // loadTileFile only: the bytes never arrived
    kNotVxtl,          // no "VXTL" magic, or too short to hold it
    kWrongVersion,     // a .vxtl, but not v2
    kBadHeader,        // a §3 field outside the contract, incl. a short header
    // A well-formed §3 header that declares a FEATURE this build does not
    // implement -- today, a `flags` bit outside kFineFlagsKnown. Split out of
    // kBadHeader because the two point in OPPOSITE directions: kBadHeader says
    // the file is broken, kUnknownFeature says the READER is old, and a reason
    // code that cannot tell them apart sends whoever reads the log to check
    // the tile when they should be checking their binary. (That is not
    // hypothetical: a game module built before the water plane existed
    // rejected four perfectly valid bake_ver-12 tiles as "bad-header", and the
    // message pointed away from the cause for the whole of the diagnosis.)
    //
    // NOT a softer verdict: the tile is still refused whole, for the reason
    // fineCodecKnown's comment gives -- bytes this decoder does not understand
    // must never be half-read into terrain. Only the EXPLANATION changes.
    kUnknownFeature,
    kUnknownCodec,     // `codec` is neither CODEC_RAW nor CODEC_ZSTD
    kNoDecompressor,   // CODEC_ZSTD declared and no decompressor was injected
    kBadSectionTable,  // §3 table: out of bounds, overlapping, duplicated, missing
    kBadBlockIndex,    // a §4 index entry is structurally impossible
    kBadBlockCoords,   // decode: block coords outside the grid, or no flow plane
    kDecompressFailed, // decode: the injected decompressor rejected the frame
    kBadPayload,       // decode: payload length wrong for the block's mode
    kValueOutOfRange,  // decode: a reconstructed value left the element's range
    kBadBasinTable,    // §P1 table: wrong version/row size, bad count, bad row
    kBadHeadwaterTable, // SECTION_HEADWATERS: wrong version/row size, bad row/order
    // The bytes for this block were never fetched. NOT an error in the data and
    // NOT a decode failure -- the tile is fine, this client simply does not hold
    // that part of it yet. It exists as its own code because the ONLY other
    // thing a partial tile could say is "0", and 0 on the elevation plane is
    // SEA LEVEL: a different world, not a degraded one (tilestreaming.h states
    // this as the rule the whole fine tier is gated on). A caller that gets
    // this must block or refuse; it must never substitute a value.
    kBlockNotResident,
};

// Stable short name, for logs. Never null.
const char* fineErrorName(FineError e);

// CAN RE-READING THE SAME SOURCE PLAUSIBLY CHANGE THIS ANSWER?
//
// This is the distinction a retrying loader has to be able to make and could
// not make before, and its absence is what turned one stale binary into a
// process that re-read, re-rejected and re-logged the same four tiles every
// frame for the length of a session while showing nothing on screen.
//
//   true  -- the SOURCE was incomplete or moved under us: bytes that never
//            arrived (kBlockNotResident: fetch more), or a read that failed
//            against a file being written, locked, or truncated mid-download
//            (kFileUnreadable). Retrying is the correct response and is the
//            only way these ever resolve.
//   false -- the bytes were read and are STRUCTURALLY refused. Reading the
//            identical bytes again produces the identical verdict, so a retry
//            is a spin: it cannot make progress, it cannot fail loudly, and it
//            costs a log line and a seek every time round. A caller must stop
//            and surface the refusal instead. (What CAN change the answer is
//            the FILE changing -- a re-bake, a completed download, or a
//            rebuilt reader. That is a new fact to notice, not a retry.)
//
// Deliberately a total switch over the enum with no default, so adding a
// FineError forces a decision here rather than silently defaulting to one of
// two behaviours that are equally wrong for half the codes.
constexpr bool fineErrorIsTransient(FineError e) {
    switch (e) {
    case FineError::kFileUnreadable:
    case FineError::kBlockNotResident:
        return true;
    case FineError::kNone:
    case FineError::kNotVxtl:
    case FineError::kWrongVersion:
    case FineError::kBadHeader:
    case FineError::kUnknownFeature:
    case FineError::kUnknownCodec:
    case FineError::kNoDecompressor:
    case FineError::kBadSectionTable:
    case FineError::kBadBlockIndex:
    case FineError::kBadBlockCoords:
    case FineError::kDecompressFailed:
    case FineError::kBadPayload:
    case FineError::kValueOutOfRange:
    case FineError::kBadBasinTable:
    case FineError::kBadHeadwaterTable:
        return false;
    }
    return false;
}

// The §3 header fields a REFUSAL MESSAGE needs, read with NO validation at all.
//
// Separate from FineTileHeader on purpose: that struct is the output of a
// parse that SUCCEEDED, and every interesting refusal happens before there is
// one. These are the numbers a human needs to place the blame -- what the file
// says it is -- gathered even when the file is about to be thrown away.
struct FineHeaderFacts {
    bool magicOk = false;      // the four "VXTL" bytes were there
    uint16_t formatVersion = 0;
    // True when formatVersion == kFineFormatVersion, i.e. the v2 header layout
    // applies and the fields below were actually read. On a v1 (or unknown)
    // file they are left at 0 rather than read out of a layout that does not
    // hold: `bake_ver` is a v2 extension and reading it positionally from a v1
    // file would invent a number and put it in an error message.
    bool v2Fields = false;
    uint16_t bakeVer = 0;
    uint16_t flags = 0;
    uint8_t codec = 0;
    // The bits `flags` sets that this build has no implementation for. Nonzero
    // is the signature of a reader older than its tiles.
    uint16_t unknownFlagBits() const {
        return static_cast<uint16_t>(flags & static_cast<uint16_t>(~kFineFlagsKnown));
    }
};

// Fills `out` from the head of a .vxtl. False (and `out` left default) only if
// `head` is null, shorter than kFineHeaderBytes, or lacks the magic -- in
// which case there is nothing to say about the file beyond "not a .vxtl".
bool fineReadHeaderFacts(const uint8_t* head, size_t headLen, FineHeaderFacts& out);

// One sentence explaining a refusal, naming BOTH numbers wherever the reader's
// own limits are half the story. Never empty, safe for any (error, facts)
// pair including a default-constructed FineHeaderFacts.
//
// This lives in voxel-core rather than in the host's log call because it is
// the part that has to be RIGHT and the host is the part that cannot be unit
// tested -- checking it needed a UE editor, a real bake and a deliberately
// stale binary, which is why nobody ever checked it.
std::string fineDescribeRejection(FineError e, const FineHeaderFacts& facts);

// mm per LSB for §3 `quant`. 0 for an unsupported value.
constexpr int32_t fineQuantMm(uint8_t quant) {
    return quant == 1 ? 100 : (quant == 2 ? 250 : 0);
}

// The v2 fixed header, exactly as laid out in §3 (little-endian, packed).
struct FineTileHeader {
    uint64_t seed = 0;
    int32_t x = 0, y = 0;   // COARSE tile coords; footprint == s1 tile (x, y)
    uint8_t scale = kFineTileScale;
    uint16_t size = static_cast<uint16_t>(kFineTileSize);
    uint8_t blockLog2 = 8;  // 8 -> 256x256 fine px per block
    uint8_t predictor = kPredMed;
    uint8_t quant = 1;      // 1 = 100 mm/LSB, 2 = 250 mm/LSB
    uint8_t codec = kCodecRaw;
    uint16_t bakeVer = 0;
    uint16_t flags = 0;
    int32_t baseOffsetMm = 0;
    uint8_t parentScale = 0; // 0 = absolute. Reserved for a residual ladder.
};

// One §4 block-index entry.
struct FineBlockEntry {
    uint64_t offset = 0;   // into the matching DATA section
    uint32_t compLen = 0;  // 0 when mode == CONSTANT
    uint8_t mode = kBlockConstant;
    int16_t constCp = 0;   // the whole block's value when CONSTANT
    uint8_t residBits = 16; // 16 or 32 (§5) -- REQUIRED, not an optimisation
};

// ---------------------------------------------------------------------------
// A SUBSET of a .vxtl file's bytes, addressed by ABSOLUTE FILE OFFSET.
//
// This is what lets a client hold part of a tile. The whole-file case is one
// segment covering [0, fileSize) and is what `whole()` builds, so every
// existing caller keeps byte-for-byte identical behaviour; a sliced client
// holds the preamble plus whichever block payloads it fetched.
//
// ADDRESSING BY FILE OFFSET, not by section, is the whole trick: FineTile
// already resolves a block to `<data section offset> + <entry.offset>`, an
// absolute file position, so nothing above this class has to learn a second
// coordinate system. A range fetched off disk or out of a 206 response drops
// straight in at the offset it was requested from.
//
// THE ONE THING THIS CLASS MUST NEVER DO is answer a read it cannot satisfy.
// `span()` returns nullptr for bytes that were not fetched -- it does not
// return zeroes, and it does not return a short buffer. Every caller is
// therefore forced to have a not-resident branch, which is the property that
// keeps an unfetched block from decoding as flat terrain at sea level.
class FineTileBytes {
public:
    FineTileBytes() = default;

    // The whole file as one segment -- the shape every pre-slicing caller has.
    static FineTileBytes whole(std::vector<uint8_t> bytes) {
        FineTileBytes b;
        b.fileSize_ = bytes.size();
        b.whole_ = true;
        if (!bytes.empty()) {
            b.segs_.push_back(Segment{0, std::move(bytes)});
        }
        return b;
    }

    // An empty store for a file KNOWN to be `fileSize` bytes long. The length
    // has to be known up front because FineTile::parse validates that the
    // section table covers the file exactly -- a check that catches a truncated
    // download, and which a partial reader must not lose. A range fetcher gets
    // the number from `Content-Range: bytes a-b/<total>` or from the file's
    // size on disk.
    static FineTileBytes forFile(uint64_t fileSize) {
        FineTileBytes b;
        b.fileSize_ = fileSize;
        return b;
    }

    // Adds one fetched range at absolute file offset `offset`. Returns false
    // (no state change) if it runs past `fileSize()` or OVERLAPS a segment
    // already held: overlapping segments would mean two different answers for
    // one byte, and the only way that happens is a mis-planned fetch or a
    // server that answered a different range than it was asked for. Refusing
    // is how that becomes visible instead of becoming terrain. Adjacent (not
    // overlapping) segments are merged.
    bool addSegment(uint64_t offset, std::vector<uint8_t> data);

    // Pointer to `len` contiguous bytes at file offset `off`, or nullptr if any
    // of them was not fetched. A zero-length request is answered nullptr: there
    // is no such thing as a useful empty payload here (a block that owns no
    // bytes is CONSTANT and is served from the index), so returning a
    // dereferenceable pointer for one would only ever paper over a caller bug.
    const uint8_t* span(uint64_t off, uint64_t len) const;

    bool covers(uint64_t off, uint64_t len) const { return span(off, len) != nullptr; }

    uint64_t fileSize() const { return fileSize_; }
    // How many bytes are actually held. THIS, not fileSize(), is what a partial
    // client pays in RAM, and it is the number the measurement reports.
    uint64_t residentBytes() const;
    size_t segmentCount() const { return segs_.size(); }
    // True for a store built by whole(): every byte of the file is held.
    bool isWhole() const { return whole_; }

private:
    struct Segment {
        uint64_t offset = 0;
        std::vector<uint8_t> data;
        uint64_t end() const { return offset + data.size(); }
    };
    // Sorted by offset, non-overlapping, non-adjacent (adjacent pairs are
    // merged on insert). Small -- a ground-only fetch holds 2 segments, a
    // three-plane one about 10 -- so a linear scan beats anything cleverer and
    // keeps the ordering invariant checkable by eye.
    std::vector<Segment> segs_;
    uint64_t fileSize_ = 0;
    bool whole_ = false;
};

// A parsed v2 fine tile. Owns the file bytes and the block index; block
// PAYLOADS are decoded on demand and never all at once.
//
// That is not an optimisation, it is §4's point: blocks are independent (one
// frame each, no shared dictionary, no cross-block prediction) so the client
// decodes only the ~0.23 km^2 blocks it needs. A whole 8192^2 int16 lattice is
// 134 MB; one decoded block is 128 KB.
//
// Every decode* method is const and allocation-only-into-the-caller's-buffer,
// so a parsed FineTile is immutable and safe to share across threads.
class FineTile {
public:
    // Exact parse: validates magic, version, scale, size, predictor, quant,
    // codec, reserved/pad bytes, the section table (in bounds, non-overlapping,
    // required sections present, no bytes past the last section) and every
    // block-index entry, including -- under CODEC_RAW -- that each block's
    // stored length is exactly what the mode implies. Returns nullopt on any
    // mismatch, and sets *err to why; the same all-or-nothing shape
    // TileData::parse has for v1.
    //
    // `decompressor` is the injected CODEC_ZSTD reader. Leaving it default
    // means CODEC_RAW only: a tile declaring CODEC_ZSTD is then refused here,
    // with err == kNoDecompressor, and is never half-loaded. It is kept for
    // the tile's lifetime, so whatever it borrows must outlive the FineTile.
    //
    // NOTE the length trap this cannot check for you (§4): under CODEC_RAW a
    // literal int16 plane and a resid_bits=16 residual plane are the SAME
    // LENGTH, so passing the length check is not evidence the payload was read
    // as the right thing. Only `mode` distinguishes them. Under CODEC_ZSTD
    // even the length check is gone -- comp_len is the compressed size and
    // constrains nothing -- which is why decode insists the frame expand to
    // exactly the size the header implies.
    static std::optional<FineTile> parse(std::vector<uint8_t> bytes,
                                         const FineDecompressor& decompressor = {},
                                         FineError* err = nullptr);
    static std::optional<FineTile> parse(const uint8_t* data, size_t size,
                                         const FineDecompressor& decompressor = {},
                                         FineError* err = nullptr);

    // PARTIAL parse: the same all-or-nothing structural check, over a tile of
    // which only some bytes were fetched (see FineTileBytes).
    //
    // WHAT MUST BE PRESENT: the header, the section table, every block index
    // section the flags declare, and the basin table. That is the "preamble" --
    // 62-67 KB in four disjoint regions on a shipped tile, against 32-56 MB of
    // file -- and it is exactly the set this function validates. Anything
    // missing from it is kBlockNotResident, distinguishable from a corrupt
    // tile, because "I did not fetch enough" and "these bytes are wrong" call
    // for opposite responses: fetch more, versus discard and never retry.
    //
    // WHAT MAY BE ABSENT: any and all DATA-section bytes. A block whose payload
    // was not fetched is not an error here and is not decoded to zero later --
    // decodeElevBlock/decodeFlowBlock/decodeWaterBlock refuse it with
    // kBlockNotResident, and *BlockResident() below answers the question up
    // front so a caller can gate instead of discovering it mid-query.
    //
    // Note that CONSTANT blocks are resident with zero bytes fetched: they own
    // no data-section entry at all (their (offset=0, comp_len=0) is not a
    // range), so they are served from `const_cp` in the index. On the shipped
    // water plane that is 72-87% of the tile.
    static std::optional<FineTile> parsePartial(FineTileBytes bytes,
                                                const FineDecompressor& decompressor = {},
                                                FineError* err = nullptr);

    // Splices later-fetched bytes into an ALREADY PARSED tile, so a client can
    // start from the preamble and pull blocks in as it needs them without
    // re-parsing. Returns false (no state change) if a segment overlaps bytes
    // already held or runs past the file -- see FineTileBytes::addSegment.
    //
    // THE ONE NON-CONST METHOD ON THIS CLASS, and it is why the class comment's
    // "immutable, safe to share across threads" claim needs qualifying: a
    // FineTile is immutable EXCEPT for this call, so a host that shares one
    // across threads must make this call exclusive with every reader. The UE
    // streamer does that with the same write lock it already takes for load and
    // evict; a caller that cannot must fetch everything before publishing.
    bool addFetchedBytes(uint64_t fileOffset, std::vector<uint8_t> data);

    const FineTileHeader& header() const { return h_; }
    uint64_t seed() const { return h_.seed; }
    int32_t tileX() const { return h_.x; }
    int32_t tileY() const { return h_.y; }
    uint32_t size() const { return h_.size; }
    uint32_t blockLog2() const { return h_.blockLog2; }
    uint32_t blockDim() const { return 1u << h_.blockLog2; }
    uint32_t blocksPerAxis() const { return static_cast<uint32_t>(h_.size) >> h_.blockLog2; }
    uint32_t blockPixelCount() const { return blockDim() * blockDim(); }
    uint32_t blockCount() const { return blocksPerAxis() * blocksPerAxis(); }
    int32_t quantMm() const { return fineQuantMm(h_.quant); }
    int32_t baseOffsetMm() const { return h_.baseOffsetMm; }
    bool hasFlow() const { return (h_.flags & kFineFlagFlowPresent) != 0; }
    // True when the tile carries a basin table AT ALL. An empty table with
    // this true means "no basins here"; this false means "baked before the
    // registry existed", and the two must not be conflated.
    bool hasBasins() const { return (h_.flags & kFineFlagBasinsPresent) != 0; }
    // True when the tile carries a water plane AT ALL. Same distinction as
    // hasBasins(): an all-dry plane with this true means "no rivers here",
    // this false means "baked before the water plane existed".
    bool hasWater() const { return (h_.flags & kFineFlagWaterPresent) != 0; }
    // True when the tile carries a headwater table AT ALL. Same distinction
    // again: an empty table with this true means "no reach starts here", this
    // false means "baked before headwaters existed" (bake_ver < 24).
    bool hasHeads() const { return (h_.flags & kFineFlagHeadsPresent) != 0; }
    // True when the tile carries the bathymetry PAIR at all. Same distinction
    // once more: an all-dry depth plane with this true means "surveyed, no lakes
    // here", this false means "baked before bathymetry existed" (bake_ver < 27).
    // One flag, both planes -- there is no tile with one of them.
    bool hasBathy() const { return (h_.flags & kFineFlagBathyPresent) != 0; }
    // The registry, ordered by (min_y, min_x) of extent with ids 0..n-1, so
    // `basins()[i].basinId == i` and a row means the same basin in every
    // process. Empty when hasBasins() is false.
    //
    // MUST BE READ TOGETHER WITH basinsResident(). On a partially fetched tile
    // this is also empty when the table simply was not downloaded, and the two
    // are opposite facts: "this tile was surveyed and holds no lakes" versus
    // "we do not know what lakes this tile holds". A caller that reads only
    // this container places no water in either case, and one of those is wrong.
    const std::vector<BasinEntry>& basins() const { return basins_; }

    // The headwater points, ordered by (y, x), each with the discharge at that
    // cell. Empty when hasHeads() is false, and -- exactly as for basins() --
    // ALSO empty on a partial tile whose table was not fetched, which is why
    // headsResident() exists beside it. A faucet list that is empty because
    // nothing was downloaded looks identical to a dry tile otherwise.
    const std::vector<HeadEntry>& heads() const { return heads_; }

    // --- which PREAMBLE sections this client actually holds -----------------
    //
    // On a whole-file tile all three are true whenever the matching has*() is.
    // On a sliced one they say what was fetched, and they exist so that "not
    // downloaded" can never be read as "empty" -- the same distinction hasFlow()
    // / hasBasins() / hasWater() draw between "absent from the format" and
    // "present but empty", one level further down.
    //
    // A false here means every block of that plane answers NOT-RESIDENT: with
    // no index there is no way even to know where the blocks are, let alone
    // what is in them.
    bool flowIndexResident() const { return flowIndex_.size() == blockCount(); }
    bool waterIndexResident() const { return waterIndex_.size() == blockCount(); }
    // Per PLANE, not per pair: the two bathymetry indices are separate sections
    // and a ranged client can hold one and not the other (they are 20 KB apart
    // with a data section in between). The tile-level "both or neither" rule is
    // about what the FILE carries; this is about what was fetched.
    bool bathyDepthIndexResident() const { return bathyDepthIndex_.size() == blockCount(); }
    bool bathyShoreIndexResident() const { return bathyShoreIndex_.size() == blockCount(); }
    bool basinsResident() const { return basinsResident_; }
    bool headsResident() const { return headsResident_; }

    const std::vector<FineBlockEntry>& elevIndex() const { return elevIndex_; }
    const std::vector<FineBlockEntry>& flowIndex() const { return flowIndex_; }
    const std::vector<FineBlockEntry>& waterIndex() const { return waterIndex_; }
    const std::vector<FineBlockEntry>& bathyDepthIndex() const { return bathyDepthIndex_; }
    const std::vector<FineBlockEntry>& bathyShoreIndex() const { return bathyShoreIndex_; }

    // FILE offset of each plane's data section -- what an index entry's
    // `offset` is relative to, and therefore what a range fetcher must add to
    // it to get a byte position. 0 when the tile does not carry the plane.
    uint64_t elevDataOffset() const { return elevDataOff_; }
    uint64_t flowDataOffset() const { return flowDataOff_; }
    uint64_t waterDataOffset() const { return waterDataOff_; }
    uint64_t bathyDepthDataOffset() const { return bathyDepthDataOff_; }
    uint64_t bathyShoreDataOffset() const { return bathyShoreDataOff_; }

    uint8_t codec() const { return h_.codec; }
    // The decompressor this tile was parsed with. Empty for a CODEC_RAW tile.
    const FineDecompressor& decompressor() const { return dec_; }

    // §2's formula, in int64 so the multiply cannot overflow before the sum.
    int32_t elevationMmFromCp(int16_t cp) const {
        const int64_t mm = static_cast<int64_t>(h_.baseOffsetMm) +
                           static_cast<int64_t>(cp) * static_cast<int64_t>(quantMm());
        return static_cast<int32_t>(mm);
    }

    // Decodes one block's control points into `out` (resized to
    // blockPixelCount(), row-major, x fastest). Returns false if the block
    // coords are out of range or the payload is corrupt, setting *err to why.
    // Pure: same bytes in, same values out, on every platform.
    //
    // ONE BLOCK, ONE FRAME (§4). This decompresses exactly the requested
    // block's frame into a buffer sized from the header, touching no other
    // block and carrying no state between calls -- which is the whole reason
    // the client can pull the ~0.23 km^2 it needs instead of 134 MB.
    bool decodeElevBlock(uint32_t bx, uint32_t by, std::vector<int16_t>& out,
                         FineError* err = nullptr) const;
    // Same for the optional §6 flow plane (one uint8 per fine pixel). False
    // when the tile carries no flow plane.
    bool decodeFlowBlock(uint32_t bx, uint32_t by, std::vector<uint8_t>& out,
                         FineError* err = nullptr) const;
    // Same for the optional water plane (int16 cp per fine pixel, P2). False
    // when the tile carries no water plane. Values are raw control points:
    // compare against `kWaterDryDepth` first, then convert with
    // `waterMmFromDepth`.
    bool decodeWaterBlock(uint32_t bx, uint32_t by, std::vector<int16_t>& out,
                          FineError* err = nullptr) const;
    // The bathymetry pair (bake_ver 27), int16 per fine pixel, decoded through
    // exactly the same block machinery as the two above -- same index layout,
    // same MODE_CONSTANT / MODE_CODED / MODE_RAW handling, the same shared
    // decodeBlockPayload. False when the tile carries no bathymetry, when the
    // coords are out of range, or when this client never fetched the bytes.
    //
    // Values are RAW WIRE UNITS. Convert with bathyDepthMm / bathyShoreMm, and
    // test dryness with bathyDepthIsDry, rather than multiplying by 10 or 100 at
    // the call site -- see the comment on those constants.
    bool decodeBathyDepthBlock(uint32_t bx, uint32_t by, std::vector<int16_t>& out,
                               FineError* err = nullptr) const;
    bool decodeBathyShoreBlock(uint32_t bx, uint32_t by, std::vector<int16_t>& out,
                               FineError* err = nullptr) const;

    // Water-surface elevation from a water control point, or `kNoWaterMm` for
    // the dry sentinel. The sentinel test lives HERE rather than at each call
    // site precisely so it cannot be forgotten at one of them.
    // Water surface from a stored DEPTH and the ground control point under it.
    // The two must come from the same pixel; see the header note on
    // kSectionWaterData for why depth is stored rather than an absolute
    // surface, and why the ground here must be the TILE LATTICE's rather than
    // an amplified surface.
    // `groundMm` is the RECONSTRUCTED SURFACE at this pixel -- the spline
    // evaluated on the control lattice, which is what Amplifier produces from
    // an ITileSampler -- in absolute millimetres.
    //
    // NOT `elevationMmFromCp(cp)`. A control point is not the surface: the
    // prefilter stands it up to 5.6 m away from the sample it interpolates
    // (measured on this world; p99 of |cp - sample| = 4.4 m). Adding a depth to
    // the lattice instead of to the spline is the mistake this signature exists
    // to make impossible, and it produced a visibly non-monotone reach the
    // first time it was written.
    //
    // Nor the AMPLIFIED surface: the amplifier's rills and bedding sit on top
    // of the bake, and water added to them would ripple by metres on a surface
    // that is flat by definition.
    // THE LEVEL BAND. A NEGATIVE cp other than the sentinels is not "dry", it
    // is "dry HERE, and the local water level is this far BELOW the ground".
    // The bake ships it for a collar of dry pixels around every wet one so the
    // client can resolve the waterline against the ground it actually draws, at
    // 10 cm, instead of inheriting the bake's 1.875 m pixel edge -- which is
    // what makes a shoreline meet a bank in blocky multi-metre steps.
    //
    // Encoding the band as negatives is the whole compatibility argument: every
    // reader that predates it tests `< 0` and sees dry, so an old client reads a
    // new tile bit-identically. This function is the ONE place in C++ that
    // decodes the plane, which is why widening it here is the whole change.
    static int32_t waterMmFromDepth(int16_t depthCp, int32_t groundMm) {
        if (depthCp == kWaterDryDepth || depthCp == kWaterNoLevel) return kNoWaterMm;
        // Both branches are the same arithmetic -- a positive cp is a depth
        // ABOVE the ground, a band cp is a level BELOW it, and the sign already
        // says which. Keeping one expression is deliberate: two would be two
        // things to keep agreeing about the datum.
        return static_cast<int32_t>(static_cast<int64_t>(groundMm) +
                                    static_cast<int64_t>(depthCp) * kWaterDepthLsbMm);
    }

    // Is this cp a WET cell, as against dry-with-a-level or no level at all?
    // `waterMmFromDepth` returns a finite surface for both wet and banded
    // cells, so anything that means "is there water standing here" must ask
    // this and not test the surface for kNoWaterMm.
    static constexpr bool waterCpIsWet(int16_t depthCp) { return depthCp >= 0; }

    // Single control point, tile-LOCAL pixel coords. Decodes the containing
    // block on every call, so it is for tests and cold paths; anything hot
    // should go through decodeElevBlock or FineTileSampler's block cache.
    //
    // False for an out-of-range pixel, a corrupt block, OR a block whose bytes
    // were never fetched -- the three are told apart by `err`, and a caller
    // that ignores it must still not substitute a value for `cp`.
    bool controlPointAt(uint32_t lx, uint32_t ly, int16_t& cp, FineError* err = nullptr) const;

    // Both bathymetry fields at one tile-LOCAL pixel, ALREADY IN MILLIMETRES:
    // `depthMm` is kNoBathyDepthMm outside every basin extent, `shoreMm` is
    // signed (positive in water). Decodes the two containing blocks on every
    // call, so -- exactly like controlPointAt -- it is for tests and cold paths;
    // anything per-pixel should decode the blocks once and convert with
    // bathyDepthMm / bathyShoreMm.
    //
    // BOTH FIELDS FROM ONE CALL because they are always read together (depth
    // grades colour, distance sets the width of anything drawn along the shore)
    // and because a caller that took them from two calls could get them from two
    // different residency states without noticing.
    bool bathyAt(uint32_t lx, uint32_t ly, int32_t& depthMm, int32_t& shoreMm,
                 FineError* err = nullptr) const;

    // --- per-block residency ------------------------------------------------
    //
    // "Do I hold the bytes for this block?" -- answerable without decoding
    // anything, which is what makes it usable as a GATE. True in two cases and
    // it is worth being explicit about the second:
    //
    //   * the block's payload bytes were fetched, or
    //   * the block is CONSTANT, and therefore has no payload bytes to fetch.
    //     A CONSTANT block is fully served by `const_cp` in the index, so it is
    //     resident the moment the index is. This is not a special case bolted
    //     on: it is what makes a water refresh cheap, the shipped water plane
    //     being 72-87% CONSTANT.
    //
    // False for out-of-range coordinates, and false for every block of a plane
    // the tile does not carry (a flow query on a tile with no flow section).
    bool elevBlockResident(uint32_t bx, uint32_t by) const;
    bool flowBlockResident(uint32_t bx, uint32_t by) const;
    bool waterBlockResident(uint32_t bx, uint32_t by) const;
    bool bathyDepthBlockResident(uint32_t bx, uint32_t by) const;
    bool bathyShoreBlockResident(uint32_t bx, uint32_t by) const;

    // Blocks of each plane whose bytes are held (CONSTANT ones counted, per
    // above). residentBlockCount(elev) == blockCount() for a whole-file tile.
    uint32_t residentElevBlocks() const;
    uint32_t residentWaterBlocks() const;

    // What this tile costs in RAM as file bytes -- the whole file for a
    // whole-file load, only the fetched regions for a sliced one. Excludes
    // decoded block caches, which live in FineTileSampler, not here.
    uint64_t residentFileBytes() const { return bytes_.residentBytes(); }
    uint64_t fileSize() const { return bytes_.fileSize(); }
    // True when this tile holds every byte of its file.
    bool isWholeFile() const { return bytes_.isWhole(); }

private:
    // Where a block's payload lives IN THE FILE, or {0,0} when it owns no bytes
    // (CONSTANT). Shared by the residency tests and the decoders so the two can
    // never disagree about which bytes a block needs.
    bool blockFileSpan(const std::vector<FineBlockEntry>& index, uint64_t dataOff, uint32_t bx,
                       uint32_t by, uint64_t& off, uint64_t& len) const;

    FineTileBytes bytes_;
    FineTileHeader h_;
    FineDecompressor dec_{};
    std::vector<FineBlockEntry> elevIndex_, flowIndex_, waterIndex_;
    std::vector<FineBlockEntry> bathyDepthIndex_, bathyShoreIndex_;
    std::vector<BasinEntry> basins_;
    std::vector<HeadEntry> heads_;
    // Distinguishes a fetched-and-empty basin table from an unfetched one. A
    // bool rather than "basins_.empty()" precisely because an empty table is
    // legitimate and means something different -- see basins().
    bool basinsResident_ = false;
    bool headsResident_ = false;
    uint64_t elevDataOff_ = 0, elevDataLen_ = 0;
    uint64_t flowDataOff_ = 0, flowDataLen_ = 0;
    uint64_t waterDataOff_ = 0, waterDataLen_ = 0;
    uint64_t bathyDepthDataOff_ = 0, bathyDepthDataLen_ = 0;
    uint64_t bathyShoreDataOff_ = 0, bathyShoreDataLen_ = 0;
};

// Grid of v2 fine tiles for one seed, addressed in FINE tile-pixel coordinates
// (1875 mm/px): pixel (px, py) belongs to coarse tile
// (floorDiv(px, size), floorDiv(py, size)) -- the fine grid of coarse tile
// (x, y) covers fine pixels [x*size, (x+1)*size), which at the production
// size of 8192 is the same 15.36 km footprint the s1 tile covers at 30 m.
//
// elevationMm() returns the CONTROL POINT's elevation, not a spline sample.
// That is deliberate and is what makes this class useful: Amplifier consumes
// an ITileSampler and treats what it returns as the control lattice for the
// v9 cubic carrier, so pointing an Amplifier at a FineTileSampler evaluates
// docs/vxtl-v2-format.md §8's spline on this lattice using the already-shipped
// carrier -- no second implementation of the normative spline exists, and
// therefore none can drift.
//
// THREADING, and it differs from TileGridSampler on purpose. TileGridSampler
// is fully populated at init and its queries are pure reads. This one decodes
// blocks LAZILY (§4: never materialise 134 MB), so a query can mutate the
// block cache and concurrent queries are NOT safe. Call prewarm() over the
// region of interest from one thread first; once every touched block is
// resident, queries are pure reads and the class has TileGridSampler's
// contract again.
class FineTileSampler final : public ITileSampler {
public:
    // `climateSource` is optional and borrowed: the fine tier carries
    // elevation (and optionally flow), never climate, so climate() delegates
    // to the coarse sampler when one is supplied and otherwise answers the
    // documented bland default. It must outlive this sampler.
    explicit FineTileSampler(uint64_t seed, ITileSampler* climateSource = nullptr)
        : seed_(seed), climate_(climateSource) {}

    uint64_t seed() const { return seed_; }
    int32_t pixelSizeMm() const override { return tilePixelSizeMm(kFineTileScale); }

    // Registers the injected CODEC_ZSTD reader used by the byte/file loaders
    // below. Call it before loading anything: it is NOT retroactive, because a
    // FineTile keeps the decompressor it was parsed with (blocks decode lazily,
    // so the callback has to be reachable for the tile's whole life). With none
    // registered, a CODEC_ZSTD tile is refused at load with kNoDecompressor
    // rather than loaded and silently full of holes.
    void setDecompressor(const FineDecompressor& d) { dec_ = d; }
    const FineDecompressor& decompressor() const { return dec_; }

    // Stores a parsed tile keyed by its COARSE (x, y). Rejects (returns false,
    // no state change) if the tile's seed doesn't match this sampler, or if
    // its `size` differs from the already-loaded tiles': the grid stride is
    // what routes a pixel to a tile, so two edges in one sampler would make
    // addressing ambiguous rather than merely inconsistent.
    bool loadTile(FineTile tile);
    // Parse-then-store, using the registered decompressor. *err says why on
    // failure -- kNoDecompressor is a host wiring bug, everything else is bad
    // bytes, and a caller that cannot tell them apart chases the wrong one.
    bool loadTile(const std::vector<uint8_t>& bytes, FineError* err = nullptr);
    bool loadTileFile(const std::filesystem::path& path, FineError* err = nullptr);
    // Stores a tile of which only some bytes were fetched. The preamble must be
    // complete (see FineTile::parsePartial); data blocks may be absent and are
    // then answered NOT-RESIDENT rather than 0 by every read path below.
    bool loadTilePartial(FineTileBytes bytes, FineError* err = nullptr);
    // Splices later-fetched bytes into a loaded tile. False if (tx,ty) is not
    // loaded or the segment overlaps bytes already held. MUTATES a tile other
    // threads may be reading -- see FineTile::addFetchedBytes; a host that
    // shares this sampler must hold its write lock across this call.
    bool addTileBytes(int32_t tx, int32_t ty, uint64_t fileOffset, std::vector<uint8_t> data);

    size_t tileCount() const { return tiles_.size(); }
    // Grid stride in fine pixels, taken from the first loaded tile; 0 before
    // any tile is loaded.
    uint32_t tileSize() const { return tileSize_; }
    const FineTile* findTile(int32_t tx, int32_t ty) const;

    // Drops a previously loaded tile and every one of its decoded blocks,
    // freeing the memory both hold -- the eviction primitive LRU streaming
    // (voxelcore/tilestreaming.h) uses to stay under a byte budget. False (no
    // state change) if (tx, ty) was never loaded. If this was the LAST loaded
    // tile, tileSize() resets to 0 (matching the pre-any-load state) rather
    // than pinning the grid to a stride nothing justifies any more.
    bool unloadTile(int32_t tx, int32_t ty);

    // Raw lattice access in fine tile-pixel coords. False (and a counter bump)
    // when the tile isn't loaded or its block fails to decode.
    bool controlPointAt(int64_t px, int64_t py, int16_t& cp);

    // Control point at (px, py) in mm. Missing tile -> 0, as TileGridSampler
    // does, with missingTileQueries bumped so callers can fail loudly.
    int32_t elevationMm(int64_t px, int64_t py) override;
    ClimateSample climate(int64_t px, int64_t py) override;

    // Decodes every block covering the inclusive fine-pixel rect, so later
    // queries over it are pure reads. Returns false if any covered block
    // belongs to a missing tile or fails to decode (counters bump as usual).
    bool prewarm(int64_t px0, int64_t py0, int64_t px1, int64_t py1);

    size_t residentBlockCount() const;

    // --- residency, as a PURE query -----------------------------------------
    //
    // Both of these are const, decode nothing, fetch nothing and mutate
    // nothing, which is what lets a host use them as a gate on the query path
    // under a shared lock. They answer two different questions:
    //
    //   blockBytesResident -- do we hold this block's FILE BYTES (or is it
    //       CONSTANT and needs none)? i.e. could it be decoded without fetching.
    //   blockDecoded       -- is it already in the decoded block cache? i.e.
    //       could it be READ without mutating anything.
    //
    // A host whose worker threads share this sampler must gate on
    // blockDecoded(): a query that would have to decode is a WRITE, and the
    // shared read lock does not survive one. blockBytesResident() is what the
    // loader checks to decide whether it still has bytes to fetch.
    bool blockDecoded(int64_t px, int64_t py) const;
    bool blockBytesResident(int64_t px, int64_t py) const;

    // --- what residency actually costs in RAM -------------------------------
    //
    // Reported separately because they behave differently and the second one
    // dominates: file bytes are what a fetch pays, decoded blocks are ~5x that
    // for a fully warmed tile (a 32x32 grid of 256^2 int16 blocks is 134 MB
    // against a 32-56 MB file). Slicing the FETCH without slicing the DECODE
    // therefore leaves most of the memory on the table.
    uint64_t residentFileBytes() const;
    uint64_t decodedBlockBytes() const;

    // Same role as TileGridSampler::missingTileQueries: public so tests and
    // streaming code can assert on it directly. Atomic so a fully prewarmed
    // sampler keeps its thread-safety claim.
    std::atomic<uint64_t> missingTileQueries{0};
    // Queries whose TILE was loaded but whose BLOCK's bytes were never fetched.
    // Kept apart from blockDecodeFailures below for the reason FineError keeps
    // kBlockNotResident apart from kBadPayload: this one says "fetch more", that
    // one says "these bytes are corrupt, discard the tile". A sliced client trips
    // this routinely and is not broken; a client tripping the other one is.
    //
    // It is NOT harmless, though, and must not be treated as informational: like
    // missingTileQueries, every increment is a query that got kSeaLevelMm back
    // from elevationMm(), and on the fine tier sea level is a different world
    // rather than a degraded one. Nonzero means some gate did not cover a read.
    std::atomic<uint64_t> notResidentBlockQueries{0};
    // Blocks that passed structural validation at parse time but whose payload
    // reconstructed out of range. Separate from missingTileQueries because the
    // two mean very different things: a miss is streaming not having caught
    // up, this is corrupt bytes that got past parse.
    std::atomic<uint64_t> blockDecodeFailures{0};

private:
    struct Resident {
        FineTile tile;
        std::unordered_map<uint32_t, std::vector<int16_t>> blocks; // by*perAxis+bx
    };

    static uint64_t tileKey(int32_t tx, int32_t ty) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(tx)) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(ty));
    }
    // Resolves a fine pixel to its decoded block, decoding on first touch.
    // Sets `tile` to the owning tile and rewrites lx/ly to IN-BLOCK coords.
    const std::vector<int16_t>* blockFor(int64_t px, int64_t py, const FineTile*& tile,
                                         uint32_t& lx, uint32_t& ly);

    uint64_t seed_;
    ITileSampler* climate_ = nullptr;
    uint32_t tileSize_ = 0;
    FineDecompressor dec_{};
    std::unordered_map<uint64_t, Resident> tiles_;
};

// THE RECONSTRUCTED BAKE SURFACE at one fine pixel, in absolute millimetres --
// `spline(cp)`, and the only thing `FineTile::waterMmFromDepth` will accept as
// its `groundMm`.
//
// WHY THIS FUNCTION EXISTS AT ALL. There are three different "grounds" in this
// codebase and the water plane is only correct against the middle one:
//
//   1. the CONTROL LATTICE       `elevationMmFromCp(cp)` / `elevationMm(px,py)`
//      A prefiltered B-spline control point. NOT a surface: the prefilter
//      stands it up to 5.6 m from the sample it interpolates.
//   2. the RECONSTRUCTED SURFACE  <-- THIS. `evalCarrier` on that lattice, which
//      is what the prefilter exists to reproduce, and what the bake subtracted:
//      `depth = water_true - sample` (terrain_service/tile_codec.py
//      `water_depth_control_points`). The client adds it back here.
//   3. the AMPLIFIED SURFACE      `Amplifier::columnCached().surfaceMm`,
//      `UVoxelWorldSubsystem::GetSurfaceHeightUU`. The reconstruction PLUS the
//      rill, bedding and octave detail worldgen lays on top, plus the v16
//      horizontal warp. The bake never saw any of it.
//
// Adding a depth to (1) puts the water hundreds of millimetres off its bed and
// produced a visibly non-monotone reach the first time it was written. Adding a
// depth to (3) makes a surface that is FLAT BY DEFINITION inherit every rill
// the amplifier draws -- metres of ripple on a river. Both mistakes have been
// made on this codebase already; this function is what a caller reaches for
// instead of choosing.
//
// EVALUATED AT THE PIXEL, fx = fy = 0, because that is where the bake's depth
// was taken: `water_depth_control_points` differences two per-pixel rasters, so
// the datum is piecewise constant over a pixel -- which is also the physically
// right answer, a water surface being flat across a 1.875 m cell rather than
// draped on the sub-pixel bed.
//
// NO WARP, and that is the same call the bench carrier arms make
// (bench/stagedump.cpp, bench/bankprobe.cpp): the v16 horizontal warp is a
// worldgen term, so it belongs with the octaves on the amplified side.
//
// This is not a second implementation of the spline -- it calls carrier.h's
// production `evalCarrier` on the tier's own stencil, exactly as
// `Amplifier::evalSurface` does. There is still only one spline, so there is
// still nothing that can drift.
inline int32_t reconstructedGroundMm(ITileSampler& tiles, int64_t px, int64_t py) {
    const int64_t pxMm = tiles.pixelSizeMm();
    int64_t cp[16];
    if (carrierPrefiltersSamples(pxMm)) {
        // The 30 m tier ships SAMPLES, so the stencil has to be prefiltered
        // before it is a control lattice. The fine tier does not (carrier.h
        // static_asserts !carrierPrefiltersSamples(1875)), but branching here
        // rather than assuming keeps this usable from a coarse-tier caller.
        constexpr int64_t S = kCarrierPrefilterSpan;
        int64_t raw[S * S];
        for (int64_t b = 0; b < S; ++b) {
            for (int64_t a = 0; a < S; ++a) {
                raw[a + S * b] = tiles.elevationMm(px + kCarrierPrefilterLo + a,
                                                   py + kCarrierPrefilterLo + b);
            }
        }
        carrierPrefilterStencil(raw, cp);
    } else {
        for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 4; ++i) {
                cp[i + 4 * j] = tiles.elevationMm(px - 1 + i, py - 1 + j);
            }
        }
    }
    return static_cast<int32_t>(evalCarrier(cp, 0, 0, pxMm).heightMm);
}

// --- BULK BATHYMETRY READ -------------------------------------------------
//
// Written for one consumer and shaped by it: the client has to hand a 2D
// bathymetry field to a MATERIAL, which means filling a raster of tens of
// thousands of cells per update. FineTile::bathyAt is the wrong tool for that
// -- it decodes the two containing 256x256 blocks on EVERY call, so a 256-cell
// row would decode the same pair 256 times. sampleBathyRect decodes each
// covered block exactly once and copies the intersecting sub-rectangle out,
// which is the same "one block, one frame" economy decodeElevBlock exists for,
// applied to a rect instead of a pixel.
//
// It is also the only rect->buffer sampler in voxel-core, and deliberately
// stays a FREE FUNCTION over FineTileSampler's public surface (findTile +
// FineTile's const decoders) rather than a member: it caches nothing, mutates
// nothing, and therefore -- unlike every FineTileSampler query method -- is
// safe to call concurrently on a sampler other threads are reading.

// Written into BOTH planes for a cell that could not be read at all: no tile
// loaded, the tile predates bake_ver 27, or the block's bytes were never
// fetched. It must not collide with either plane's real range, and it does not:
// depth is kBathyDryDepth(-1) or >= 0, shore saturates at +/-kBathyShoreClampUnits
// (1000). INT16_MIN is unreachable in both.
//
// WHY A SENTINEL RATHER THAN "LEAVE IT ALONE". "No data here" and "dry land
// here" drive opposite decisions downstream -- the first must fall back to the
// screen-space path, the second must draw dry ground -- and a caller handed a
// buffer with holes it cannot see will conflate them. Same argument as
// hasBathy() vs an all-dry plane, one level further down.
inline constexpr int16_t kBathyMissing = INT16_MIN;

// Why each cell that could not be filled could not be filled. Kept apart for
// the reason FineTileSampler keeps missingTileQueries apart from
// notResidentBlockQueries: `missingTiles` means streaming has not caught up,
// `noPlane` means this world was baked before bathymetry existed, and
// `notResident` means some fetch gate did not cover this read. Only the first
// is expected to be nonzero in steady state.
struct BathyRectStats {
    uint64_t cells = 0;         // cells in the requested rect
    uint64_t filled = 0;        // cells answered from a decoded block
    uint64_t missingTiles = 0;  // owning tile not loaded in this sampler
    uint64_t noPlane = 0;       // tile loaded but carries no bathymetry (bake_ver < 27)
    uint64_t notResident = 0;   // block bytes never fetched, or index absent
    uint64_t decodeFailed = 0;  // bytes held but the payload would not reconstruct

    bool complete() const { return filled == cells; }
};

// Fills an INCLUSIVE rect of fine tile-pixel coordinates into two row-major
// int16 buffers, in RAW WIRE UNITS (convert with bathyDepthMm / bathyShoreMm;
// test dryness with bathyDepthIsDry). Cell (px, py) lands at
//   out[(py - py0) * rowStrideElems + (px - px0)]
// so a caller filling a sub-window of a larger texture passes that texture's
// row stride and a pointer to the window's first cell.
//
// Either output pointer may be null to skip that plane; the stats still count
// the cell against the plane that WAS requested. Unfilled cells are set to
// kBathyMissing in whichever buffers were supplied.
//
// Returns per-reason counts rather than a bool because the caller has to act on
// the difference (see BathyRectStats). An empty or inverted rect is not an
// error -- it fills nothing and reports cells == 0.
BathyRectStats sampleBathyRect(const FineTileSampler& tiles,
                               int64_t px0, int64_t py0, int64_t px1, int64_t py1,
                               int16_t* depthOut, int16_t* shoreOut,
                               int64_t rowStrideElems);

// Reads a whole file into memory. Returns nullopt on any I/O failure.
std::optional<std::vector<uint8_t>> readFileBytes(const std::filesystem::path& path);

// Grid of decoded tiles for a single (seed, scale), addressed in tile-pixel
// coordinates: pixel (px, py) belongs to tile (floorDiv(px,512),
// floorDiv(py,512)) at local offset (floorMod(px,512), floorMod(py,512)).
//
// Missing-tile policy (deterministic, doctrine-safe — plan §2.3 tiles are
// canonical data, never silently fabricated): a query into a tile that
// hasn't been loaded returns elevation 0 and default climate rather than
// throwing (ITileSampler's query methods are hot-path and must stay
// exception-free), but increments the public missingTileQueries counter so
// callers — tests, streaming code — can detect and fail loudly.
class TileGridSampler final : public ITileSampler {
public:
    TileGridSampler(uint64_t seed, uint8_t scale)
        : seed_(seed), scale_(scale), pixelSizeMm_(tilePixelSizeMm(scale)) {}

    uint64_t seed() const { return seed_; }
    uint8_t scale() const { return scale_; }
    int32_t pixelSizeMm() const override { return pixelSizeMm_; }

    // Stores a decoded tile, keyed by its (x, y). Rejects (returns false, no
    // state change) if the tile's seed or scale doesn't match this sampler.
    bool loadTile(TileData tile);
    // Parses then stores. Rejects on parse failure or seed/scale mismatch.
    bool loadTile(const std::vector<uint8_t>& bytes);
    // Reads a .vxtl file from disk, then parses and stores it.
    bool loadTileFile(const std::filesystem::path& path);

    size_t tileCount() const { return tiles_.size(); }

    int32_t elevationMm(int64_t px, int64_t py) override;
    ClimateSample climate(int64_t px, int64_t py) override;

    // Count of queries answered with the missing-tile default. Public by
    // design so tests/streaming code can assert directly on it.
    //
    // Atomic because this sampler is about to be shared across UE meshing
    // worker threads: tiles_ is populated only during init (all loadTile*
    // calls happen before workers start) and is never mutated afterward, so
    // elevationMm()/climate() are otherwise pure reads of immutable data —
    // this counter increment is the ONLY mutation on the hot query path, and
    // therefore the only race to guard. Relaxed ordering is fine: callers
    // only care about the final tally, not happens-before relative to other
    // memory. NOTE: this makes TileGridSampler non-copyable/non-movable
    // (verified nothing in the repo copies or moves one — grep confirms it's
    // only ever constructed in place and used by reference/pointer).
    std::atomic<uint64_t> missingTileQueries{0};

private:
    static uint64_t tileKey(int32_t tx, int32_t ty) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(tx)) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(ty));
    }
    const TileData* findTile(int64_t px, int64_t py, uint32_t& localX, uint32_t& localY) const;

    uint64_t seed_;
    uint8_t scale_;
    int32_t pixelSizeMm_;
    std::unordered_map<uint64_t, TileData> tiles_;
};

} // namespace vxc
