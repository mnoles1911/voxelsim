#pragma once
// Decoded tile storage + sampling (plan §3.1 step 2, §3.4 ITerrainSource).
// TileData parses the exact wire format produced by
// terrain-service/terrain_service/tile_codec.py (encode/decode); TileGridSampler
// serves a grid of decoded tiles for one (seed, scale) through ITileSampler,
// the same interface amplifier.h consumes for both local and remote
// ITerrainSource implementations.

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
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
};

// SECTION_BASIN_TABLE payload layout, mirroring terrain_service/tile_codec.py.
//
//   u16 table_version | u16 entry_bytes | u32 count | count * entry_bytes
//
// `entry_bytes` is redundant with the section length, and deliberately so: a
// decoder that disagrees with the encoder about the row size gets a mismatch
// it can refuse, instead of reading 33-byte records out of a 32-byte stream
// and producing plausible garbage.
inline constexpr uint16_t kBasinTableVersion = 1;
inline constexpr size_t kBasinTableHeaderBytes = 8;
inline constexpr size_t kBasinEntryBytes = 32;

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

    //: True when this basin holds standing water at equilibrium.
    bool holdsWater() const { return kind >= kBasinLakeTerminal; }
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
inline constexpr uint16_t kFineFlagsKnown =
    kFineFlagFlowPresent | kFineFlagBasinsPresent | kFineFlagWaterPresent;

// The dry sentinel in the water plane, in CONTROL-POINT units. INT16_MIN, the
// one value the elevation codec's own range can never legitimately carry as a
// water surface -- see terrain_service/tile_codec.py's WATER_DRY_CP, which
// this mirrors. A decoder must test for it BEFORE applying
// `elevationMmFromCp`, or the sentinel becomes an ordinary (very low)
// elevation and every dry pixel reads as water 3.2 km underground.
inline constexpr int16_t kWaterDryDepth = -1;

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
    kUnknownCodec,     // `codec` is neither CODEC_RAW nor CODEC_ZSTD
    kNoDecompressor,   // CODEC_ZSTD declared and no decompressor was injected
    kBadSectionTable,  // §3 table: out of bounds, overlapping, duplicated, missing
    kBadBlockIndex,    // a §4 index entry is structurally impossible
    kBadBlockCoords,   // decode: block coords outside the grid, or no flow plane
    kDecompressFailed, // decode: the injected decompressor rejected the frame
    kBadPayload,       // decode: payload length wrong for the block's mode
    kValueOutOfRange,  // decode: a reconstructed value left the element's range
    kBadBasinTable,    // §P1 table: wrong version/row size, bad count, bad row
};

// Stable short name, for logs. Never null.
const char* fineErrorName(FineError e);

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
    // The registry, ordered by (min_y, min_x) of extent with ids 0..n-1, so
    // `basins()[i].basinId == i` and a row means the same basin in every
    // process. Empty when hasBasins() is false.
    const std::vector<BasinEntry>& basins() const { return basins_; }

    const std::vector<FineBlockEntry>& elevIndex() const { return elevIndex_; }
    const std::vector<FineBlockEntry>& flowIndex() const { return flowIndex_; }
    const std::vector<FineBlockEntry>& waterIndex() const { return waterIndex_; }

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
    static int32_t waterMmFromDepth(int16_t depthCp, int32_t groundMm) {
        if (depthCp < 0) return kNoWaterMm;
        return static_cast<int32_t>(static_cast<int64_t>(groundMm) +
                                    static_cast<int64_t>(depthCp) * kWaterDepthLsbMm);
    }

    // Single control point, tile-LOCAL pixel coords. Decodes the containing
    // block on every call, so it is for tests and cold paths; anything hot
    // should go through decodeElevBlock or FineTileSampler's block cache.
    bool controlPointAt(uint32_t lx, uint32_t ly, int16_t& cp) const;

private:
    std::vector<uint8_t> bytes_;
    FineTileHeader h_;
    FineDecompressor dec_{};
    std::vector<FineBlockEntry> elevIndex_, flowIndex_, waterIndex_;
    std::vector<BasinEntry> basins_;
    uint64_t elevDataOff_ = 0, elevDataLen_ = 0;
    uint64_t flowDataOff_ = 0, flowDataLen_ = 0;
    uint64_t waterDataOff_ = 0, waterDataLen_ = 0;
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

    // Same role as TileGridSampler::missingTileQueries: public so tests and
    // streaming code can assert on it directly. Atomic so a fully prewarmed
    // sampler keeps its thread-safety claim.
    std::atomic<uint64_t> missingTileQueries{0};
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
