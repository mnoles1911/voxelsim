#include "voxelcore/tilestore.h"

#include <algorithm>
#include <cstddef>  // std::ptrdiff_t -- MSVC gets it transitively, libstdc++ does not
#include <cstdint>  // UINT64_MAX
#include <fstream>
#include <string>   // fineDescribeRejection -- std::string/std::to_string

#include "voxelcore/bytes.h"
#include "voxelcore/core.h"

namespace vxc {

std::optional<TileData> TileData::parse(const uint8_t* data, size_t size) {
    ByteReader r(data, size);

    uint8_t m0, m1, m2, m3;
    if (!r.u8(m0) || !r.u8(m1) || !r.u8(m2) || !r.u8(m3)) return std::nullopt;
    if (m0 != 'V' || m1 != 'X' || m2 != 'T' || m3 != 'L') return std::nullopt;

    uint16_t version;
    if (!r.u16(version) || version != kFormatVersion) return std::nullopt;

    TileData tile;
    if (!r.u64(tile.seed)) return std::nullopt;
    if (!r.i32(tile.x) || !r.i32(tile.y)) return std::nullopt;
    if (!r.u8(tile.scale)) return std::nullopt;

    uint16_t size16;
    if (!r.u16(size16) || size16 != kTileSize) return std::nullopt;

    tile.elevation.resize(kPixelCount);
    for (uint32_t i = 0; i < kPixelCount; ++i) {
        uint16_t raw;
        if (!r.u16(raw)) return std::nullopt;
        tile.elevation[i] = static_cast<int16_t>(raw);
    }

    for (uint32_t c = 0; c < kClimateChannels; ++c) {
        tile.climate[c].resize(kPixelCount);
        for (uint32_t i = 0; i < kPixelCount; ++i) {
            if (!r.u8(tile.climate[c][i])) return std::nullopt;
        }
    }

    if (!r.atEnd()) return std::nullopt; // trailing bytes
    return tile;
}

std::optional<std::vector<uint8_t>> readFileBytes(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return std::nullopt;
    const std::streamoff n = f.tellg();
    if (n < 0) return std::nullopt;
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (n > 0 && !f.read(reinterpret_cast<char*>(buf.data()), n)) return std::nullopt;
    return buf;
}

bool TileGridSampler::loadTile(TileData tile) {
    if (tile.seed != seed_ || tile.scale != scale_) return false;
    const uint64_t key = tileKey(tile.x, tile.y);
    tiles_.insert_or_assign(key, std::move(tile));
    return true;
}

bool TileGridSampler::loadTile(const std::vector<uint8_t>& bytes) {
    std::optional<TileData> parsed = TileData::parse(bytes.data(), bytes.size());
    if (!parsed) return false;
    return loadTile(std::move(*parsed));
}

bool TileGridSampler::loadTileFile(const std::filesystem::path& path) {
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    if (!bytes) return false;
    return loadTile(*bytes);
}

const TileData* TileGridSampler::findTile(int64_t px, int64_t py, uint32_t& localX,
                                          uint32_t& localY) const {
    const int64_t tileSize = static_cast<int64_t>(TileData::kTileSize);
    const int32_t tx = static_cast<int32_t>(floorDiv(px, tileSize));
    const int32_t ty = static_cast<int32_t>(floorDiv(py, tileSize));
    localX = static_cast<uint32_t>(floorMod(px, tileSize));
    localY = static_cast<uint32_t>(floorMod(py, tileSize));
    auto it = tiles_.find(tileKey(tx, ty));
    return it == tiles_.end() ? nullptr : &it->second;
}

int32_t TileGridSampler::elevationMm(int64_t px, int64_t py) {
    uint32_t lx, ly;
    const TileData* t = findTile(px, py, lx, ly);
    if (!t) {
        missingTileQueries.fetch_add(1, std::memory_order_relaxed);
        // A tile that is not loaded reads as SEA LEVEL, not as "zero". The
        // distinction is the point of the symbol: the fallback is a datum
        // choice (a missing tile is open ocean, the same conservative posture
        // pipeline.py's MISSING_ELEVATION_M takes on the bake side), not an
        // arbitrary default. missingTileQueries counts every one of them.
        return kSeaLevelMm;
    }
    const int64_t metres = t->elevationAt(lx, ly);
    return static_cast<int32_t>(metres * 1000);
}

ClimateSample TileGridSampler::climate(int64_t px, int64_t py) {
    uint32_t lx, ly;
    const TileData* t = findTile(px, py, lx, ly);
    if (!t) {
        missingTileQueries.fetch_add(1, std::memory_order_relaxed);
        return ClimateSample{};
    }
    ClimateSample c;
    c.temperature = t->climateAt(0, lx, ly);
    c.seasonality = t->climateAt(1, lx, ly);
    c.precipitation = t->climateAt(2, lx, ly);
    c.precipVariability = t->climateAt(3, lx, ly);
    return c;
}

std::optional<uint16_t> vxtlVersion(const uint8_t* data, size_t size) {
    ByteReader r(data, size);
    uint8_t m0, m1, m2, m3;
    if (!r.u8(m0) || !r.u8(m1) || !r.u8(m2) || !r.u8(m3)) return std::nullopt;
    if (m0 != 'V' || m1 != 'X' || m2 != 'T' || m3 != 'L') return std::nullopt;
    uint16_t v;
    if (!r.u16(v)) return std::nullopt;
    return v;
}

// ---------------------------------------------------------------------------
// .vxtl v2 decode. docs/vxtl-v2-format.md is normative; every "§" below is a
// section of it. Nothing here uses floating point, and every division is
// plain truncating C++ `/` (§7).
//
// TWO PLACES THE SPEC IS SILENT AND THIS DECODER HAD TO CHOOSE. Both are
// written down here rather than left implicit, because the Python encoder is
// built independently against the same document and a quiet disagreement is a
// corrupt world, not a build error:
//
//  (a) mode RAW (§4) is stored as LITERAL control points -- no predictor, no
//      zigzag -- int16 little-endian for elevation and one byte per pixel for
//      flow. §5 defines the predictor and §4 lists RAW beside CODED without
//      defining it. Under CODEC_RAW the alternative reading ("MED residuals,
//      just not compressed") makes RAW byte-for-byte identical to CODED and so
//      carries no information at all, which cannot be what a distinct mode is
//      for; literal samples are also the standard escape hatch for a block
//      that compression loses on. Note that the two readings produce the SAME
//      block LENGTH, so a disagreement here would not be caught by any length
//      check -- it is the one place in this format where the encoder and
//      decoder can differ silently.
//
//  (a2) the same is true, and worse, under CODEC_ZSTD: `comp_len` is then the
//      COMPRESSED length, so it constrains nothing at all about what the frame
//      holds. Length is therefore never treated as a correctness check here.
//      The mode selects the reading, and the only length assertion that
//      survives compression is "the frame must expand to exactly the size the
//      header implies", which decodeBlockPayload enforces on every frame.
//
//  (b) unknown section ids are IGNORED, not rejected. A section table exists
//      to be extended, and §7 already routes any world-affecting change
//      through bake_ver -> provider_id -> a new world, so an unknown section
//      cannot silently change terrain this decoder produces. Unknown sections
//      still take part in the bounds/overlap/no-trailing-bytes checks.
//
// Everything else here is the document read literally.
// ---------------------------------------------------------------------------

namespace {

// Reason codes are an out-param so every early return can stay a one-liner.
// `false` is the return value in every failing path; this only records why.
inline bool fail(FineError* err, FineError e) {
    if (err) *err = e;
    return false;
}

// LOCO-I / JPEG-LS median edge predictor (§5), on the three causal
// neighbours. Callers handle the block-edge cases.
inline int32_t medPredict(int32_t w, int32_t n, int32_t nw) {
    const int32_t mx = w > n ? w : n;
    const int32_t mn = w < n ? w : n;
    if (nw >= mx) return mn;
    if (nw <= mn) return mx;
    return w + n - nw;
}

// §5: residuals are zigzag-mapped as (r << 1) ^ (r >> 31), i.e. over int32,
// and this is its exact inverse. Written in unsigned arithmetic so the
// negation cannot be UB: ~(u & 1) + 1 is 0 or 0xFFFFFFFF.
inline int32_t zigzagDecode(uint32_t u) {
    return static_cast<int32_t>((u >> 1) ^ (~(u & 1u) + 1u));
}

// Exact PLAIN (post-decompression) payload length a block's mode implies, in
// bytes. 0 for CONSTANT, which owns no data bytes at all. This is the number
// the whole CODEC_ZSTD path hangs off: it is derived from the header alone,
// never from `comp_len`, so a lying comp_len cannot move it.
template <typename T>
uint64_t plainPayloadBytes(const FineBlockEntry& e, uint32_t blockPixels) {
    switch (e.mode) {
    case kBlockRaw:
        return static_cast<uint64_t>(blockPixels) * sizeof(T);
    case kBlockCoded:
        return static_cast<uint64_t>(blockPixels) * (e.residBits / 8u);
    default:
        return 0;
    }
}

// One §4 block payload, in raster order (y outer, x inner -- x fastest, the
// same order the index itself uses). T is int16_t for the elevation lattice
// and uint8_t for the §6 flow plane; lo/hi are T's representable range, and a
// reconstruction that leaves it means corrupt bytes, so the whole block is
// rejected rather than silently wrapped.
//
// `compressed` says the stored bytes are a CODEC_ZSTD frame rather than the
// payload itself. Then, and ONLY then, `dec` is invoked -- on this one block's
// frame, into a buffer this function sizes from the header (§4: one frame per
// block, no dictionary, no cross-block state, which is what makes per-block
// random access work). The callback must fill it exactly; short, long or
// malformed all come back false and reject the block.
template <typename T>
bool decodeBlockPayload(const FineBlockEntry& e, const uint8_t* data, size_t dataLen,
                        uint32_t dim, int32_t lo, int32_t hi, const FineDecompressor& dec,
                        bool compressed, T* out, FineError* err) {
    const uint32_t n = dim * dim;

    if (e.mode == kBlockConstant) {
        const int32_t v = e.constCp;
        if (v < lo || v > hi) return fail(err, FineError::kValueOutOfRange);
        std::fill(out, out + n, static_cast<T>(v));
        return true;
    }

    if (e.mode != kBlockCoded && e.mode != kBlockRaw) return fail(err, FineError::kBadPayload);
    if (e.mode == kBlockCoded && e.residBits != 16 && e.residBits != 32) {
        return fail(err, FineError::kBadPayload);
    }

    // Payload window. Re-checked here even though parse() validated it, so
    // decodeBlockPayload is safe on its own terms.
    if (e.offset > dataLen || e.compLen > dataLen - e.offset) {
        return fail(err, FineError::kBadPayload);
    }

    const uint64_t plainLen = plainPayloadBytes<T>(e, n);
    const uint8_t* payload = data + e.offset;
    size_t payloadLen = e.compLen;
    std::vector<uint8_t> inflated;

    if (compressed) {
        if (!dec.valid()) return fail(err, FineError::kNoDecompressor);
        // An empty frame cannot expand to anything, and a block that owns no
        // data bytes is by definition CONSTANT.
        if (e.compLen == 0) return fail(err, FineError::kBadPayload);
        inflated.resize(static_cast<size_t>(plainLen));
        if (!dec(payload, payloadLen, inflated.data(), inflated.size())) {
            return fail(err, FineError::kDecompressFailed);
        }
        payload = inflated.data();
        payloadLen = inflated.size();
    } else if (payloadLen != plainLen) {
        // Under CODEC_RAW the stored length IS the plain length. (parse()
        // already enforced this; keeping it here is what lets this function
        // stand on its own.)
        return fail(err, FineError::kBadPayload);
    }

    ByteReader r(payload, payloadLen);

    if (e.mode == kBlockRaw) {
        for (uint32_t i = 0; i < n; ++i) {
            if constexpr (sizeof(T) == 2) {
                uint16_t raw;
                if (!r.u16(raw)) return fail(err, FineError::kBadPayload);
                out[i] = static_cast<T>(static_cast<int16_t>(raw));
            } else {
                uint8_t raw;
                if (!r.u8(raw)) return fail(err, FineError::kBadPayload);
                out[i] = static_cast<T>(raw);
            }
        }
        return r.atEnd() ? true : fail(err, FineError::kBadPayload);
    }

    for (uint32_t y = 0; y < dim; ++y) {
        for (uint32_t x = 0; x < dim; ++x) {
            uint32_t word;
            if (e.residBits == 16) {
                uint16_t w16;
                if (!r.u16(w16)) return fail(err, FineError::kBadPayload);
                word = w16;
            } else {
                if (!r.u32(word)) return fail(err, FineError::kBadPayload);
            }
            const int32_t resid = zigzagDecode(word);

            // §5's block-edge rules. Blocks are independent: there is no
            // cross-block prediction, so "first row" and "first column" mean
            // the block's, not the tile's.
            int32_t pred;
            if (x == 0 && y == 0) {
                pred = 0;
            } else if (y == 0) {
                pred = static_cast<int32_t>(out[x - 1]);
            } else if (x == 0) {
                pred = static_cast<int32_t>(out[(y - 1) * dim]);
            } else {
                pred = medPredict(static_cast<int32_t>(out[y * dim + x - 1]),
                                  static_cast<int32_t>(out[(y - 1) * dim + x]),
                                  static_cast<int32_t>(out[(y - 1) * dim + x - 1]));
            }

            const int64_t v = static_cast<int64_t>(pred) + static_cast<int64_t>(resid);
            if (v < lo || v > hi) return fail(err, FineError::kValueOutOfRange);
            out[y * dim + x] = static_cast<T>(v);
        }
    }
    return r.atEnd() ? true : fail(err, FineError::kBadPayload);
}

struct SectionRef {
    uint64_t offset = 0;
    uint64_t length = 0;
    bool present = false;
};

// Reads one §4 index section into `out`, validating every entry as far as the
// codec allows. bytesPerSample is 2 for the elevation lattice, 1 for the flow
// plane.
//
// `compressed` changes what CAN be validated here, and it is worth being
// explicit about the difference:
//   CODEC_RAW  -- comp_len is fully determined by mode/resid_bits, so a wrong
//                 one is caught right here, at parse, before any block decodes.
//   CODEC_ZSTD -- comp_len is the frame's compressed size. Nothing but "not
//                 zero, and inside the data section" can be said about it, and
//                 the real check (the frame expands to exactly the header's
//                 size) necessarily happens at decode. So a zstd tile can pass
//                 parse and still have an individually corrupt block; that is
//                 inherent, not an oversight, and it is why the sampler keeps
//                 blockDecodeFailures separate from missing tiles.
//
// `indexData` is the index section's bytes, already resolved out of the
// (possibly partial) byte store -- this function is never handed a whole-file
// pointer plus an offset, because on a sliced tile there IS no whole-file
// pointer and "file + off" would silently address the wrong segment.
bool parseBlockIndex(const uint8_t* indexData, uint64_t indexLen,
                     uint32_t blockCount, uint32_t blockPixels, uint64_t dataLen,
                     uint32_t bytesPerSample, bool compressed,
                     std::vector<FineBlockEntry>& out) {
    if (indexLen != static_cast<uint64_t>(blockCount) * kFineBlockEntryBytes) return false;
    ByteReader r(indexData, static_cast<size_t>(indexLen));

    out.clear();
    out.reserve(blockCount);
    for (uint32_t i = 0; i < blockCount; ++i) {
        FineBlockEntry e;
        uint16_t constRaw;
        if (!r.u64(e.offset)) return false;
        if (!r.u32(e.compLen)) return false;
        if (!r.u8(e.mode)) return false;
        if (!r.u16(constRaw)) return false;
        e.constCp = static_cast<int16_t>(constRaw);
        if (!r.u8(e.residBits)) return false;
        for (int p = 0; p < 4; ++p) {
            uint8_t pad;
            if (!r.u8(pad) || pad != 0) return false; // §4: pad must be 0
        }

        const uint64_t expectRaw = static_cast<uint64_t>(blockPixels) * bytesPerSample;
        switch (e.mode) {
        case kBlockConstant:
            // §4: comp_len is 0 when mode == CONSTANT. The block costs zero
            // bytes, so it can neither be truncated nor overrun.
            if (e.compLen != 0) return false;
            // const_cp is i16 ON THE WIRE but carries one element of the
            // TARGET plane, and for the §6 flow plane that element is an
            // UNSIGNED 0..255 byte -- 0xFF means log2=31 with all three flag
            // bits, never -1. Valid data never reaches the negative half, so
            // this only ever fires on a corrupt file; reject it here rather
            // than let a narrowing conversion wrap -1 into a perfectly
            // plausible 255. (The same trap was found and fixed on the Python
            // encoder side, where a numpy assignment was doing the wrapping.)
            if (bytesPerSample == 1 && (e.constCp < 0 || e.constCp > 255)) return false;
            break;
        case kBlockCoded: {
            if (e.residBits != 16 && e.residBits != 32) return false;
            // Under CODEC_RAW the residual stream length is fully determined:
            // one resid_bits-wide word per pixel, no compression to hide it.
            const uint64_t expect = static_cast<uint64_t>(blockPixels) * (e.residBits / 8u);
            if (compressed ? (e.compLen == 0) : (e.compLen != expect)) return false;
            break;
        }
        case kBlockRaw:
            // See (a) at the top of this block: literal samples.
            if (compressed ? (e.compLen == 0) : (e.compLen != expectRaw)) return false;
            break;
        default:
            return false;
        }

        if (e.offset > dataLen || e.compLen > dataLen - e.offset) return false;
        out.push_back(e);
    }
    return r.atEnd();
}

// SECTION_BASIN_TABLE (watershed plan P1, bake_ver 8). A flat table, so this
// is a straight read -- but every field is CHECKED, because a basin row is a
// gameplay instruction: it says "flood-fill from here, up to this level", and
// a bbox reaching outside the tile or a surface above its own spill would put
// water where there is none. `tileSize` is the grid edge the row must fit in.
bool parseBasinTable(const uint8_t* tableData, uint64_t len, uint32_t tileSize, int32_t tileX,
                     int32_t tileY, std::vector<BasinEntry>& out) {
    if (len < kBasinTableHeaderBytes) return false;
    ByteReader r(tableData, static_cast<size_t>(len));
    uint16_t version, entryBytes;
    uint32_t count;
    if (!r.u16(version)) return false;
    if (version != kBasinTableVersionV1 && version != kBasinTableVersionV2) return false;
    const bool v2 = version == kBasinTableVersionV2;
    // A row size this build does not know is bytes written by a different
    // revision of the table. Refusing beats reading 81-byte records out of an
    // 80-byte stream and getting plausible garbage. CHECKED AS A PAIR with the
    // version: a v2 label over 32-byte rows is not "an old table relabelled",
    // it is bytes neither revision wrote, and reading it either way is a guess.
    const uint64_t rowBytes = v2 ? kBasinEntryBytesV2 : kBasinEntryBytes;
    if (!r.u16(entryBytes) || entryBytes != rowBytes) return false;
    if (!r.u32(count)) return false;
    if (len != kBasinTableHeaderBytes + static_cast<uint64_t>(count) * rowBytes) {
        return false;
    }
    // Where this tile's interior pixel (0,0) sits in the world, for validating
    // the v2 absolute fields against the tile-local ones. int64 throughout: the
    // product overflows int32 at |tile| >= 262,144, and a wrapped origin would
    // turn a bounds check into a coin toss.
    const int64_t worldOx = static_cast<int64_t>(tileX) * static_cast<int64_t>(tileSize);
    const int64_t worldOy = static_cast<int64_t>(tileY) * static_cast<int64_t>(tileSize);

    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        BasinEntry b;
        uint32_t spill = 0, surface = 0;
        b.tableVersion = version;
        if (!r.u16(b.basinId)) return false;
        if (!r.u16(b.seedX) || !r.u16(b.seedY)) return false;
        if (!r.u16(b.bboxX0) || !r.u16(b.bboxY0) || !r.u16(b.bboxX1) || !r.u16(b.bboxY1)) {
            return false;
        }
        if (!r.u16(b.outletX) || !r.u16(b.outletY)) return false;
        if (!r.u32(spill) || !r.u32(surface)) return false;
        b.spillMm = static_cast<int32_t>(spill);
        b.surfaceMm = static_cast<int32_t>(surface);
        if (!r.u8(b.kind) || b.kind >= kBasinKindCount) return false;
        for (int k = 0; k < 5; ++k) {
            uint8_t reserved;
            if (!r.u8(reserved) || reserved != 0) return false;
        }

        // Ids are 0..n-1 in order, because the client INDEXES by id and the
        // bake orders basins by (min_y, min_x) of extent so the id is a pure
        // function of the surface. A gap or a repeat means two processes could
        // disagree about which basin is "3".
        //
        // THAT ID IS TILE-LOCAL AND ALWAYS WAS. Two tiles sharing a lake number
        // it differently, because each numbers only what it can see; v2's
        // `globalId` is the one that says they are the same lake, which is why
        // the two could coexist without breaking this rule.
        if (b.basinId != i) return false;
        if (b.bboxX0 > b.bboxX1 || b.bboxY0 > b.bboxY1) return false;
        if (b.bboxX1 >= tileSize || b.bboxY1 >= tileSize) return false;
        if (b.seedX < b.bboxX0 || b.seedX > b.bboxX1) return false;
        if (b.seedY < b.bboxY0 || b.seedY > b.bboxY1) return false;
        // Water standing above its own outlet is not a lake, it is a bug: the
        // outlet would carry the excess away.
        if (b.surfaceMm > b.spillMm) return false;

        if (v2) {
            uint32_t floor = 0, wx0 = 0, wy0 = 0, wx1 = 0, wy1 = 0, wox = 0, woy = 0;
            if (!r.u64(b.globalId)) return false;
            if (!r.u64(b.capacityLitres)) return false;
            if (!r.u32(floor)) return false;
            if (!r.u32(wx0) || !r.u32(wy0) || !r.u32(wx1) || !r.u32(wy1)) return false;
            if (!r.u32(wox) || !r.u32(woy)) return false;
            if (!r.u8(b.spanFlags)) return false;
            if ((b.spanFlags & ~kBasinSpanCrossesTile) != 0) return false;
            for (int k = 0; k < 3; ++k) {
                uint8_t reserved;
                if (!r.u8(reserved) || reserved != 0) return false;
            }
            b.floorMm = static_cast<int32_t>(floor);
            b.worldX0 = static_cast<int32_t>(wx0);
            b.worldY0 = static_cast<int32_t>(wy0);
            b.worldX1 = static_cast<int32_t>(wx1);
            b.worldY1 = static_cast<int32_t>(wy1);
            b.worldOutletX = static_cast<int32_t>(wox);
            b.worldOutletY = static_cast<int32_t>(woy);

            if (b.worldX0 > b.worldX1 || b.worldY0 > b.worldY1) return false;
            // THE RUNTIME'S KEY CONTRACT (basinledger.h): bit 63 is that
            // type's "tile-local v1 key" tag and 0 is its "not a basin", so an
            // id violating either would be silently dropped by the ledger --
            // no lake, no error, at the far end of a save/replicate path.
            // Refused here, where the bytes arrive, rather than there.
            if (b.globalId == 0 || (b.globalId & (uint64_t(1) << 63)) != 0) return false;
            // The floor is under the water by definition. Above the surface
            // means the two came off different components, and a client would
            // then compute a negative standing depth from the row.
            if (b.floorMm > b.surfaceMm) return false;
            // THE ANCHOR IS A CELL OF THIS COMPONENT, so it lies in the
            // component's own extent. If it does not, the identity and the
            // extent describe two different basins -- and since the client's
            // union rule reads both, it would merge the wrong pair.
            const int64_t fx = b.globalIdWorldX(), fy = b.globalIdWorldY();
            if (fx < b.worldX0 || fx > b.worldX1 || fy < b.worldY0 || fy > b.worldY1) {
                return false;
            }
            // The clipped view must be the world view seen through this tile:
            // the local bbox, mapped out to world coordinates, has to lie
            // inside the unclipped one. This is the check that catches a row
            // built with the wrong tile origin -- which would put a lake a
            // whole tile away from where the client floods it.
            if (worldOx + b.bboxX0 < b.worldX0 || worldOx + b.bboxX1 > b.worldX1) return false;
            if (worldOy + b.bboxY0 < b.worldY0 || worldOy + b.bboxY1 > b.worldY1) return false;
        }
        out.push_back(b);
    }
    return r.atEnd();
}

// SECTION_HEADWATERS (water re-architecture Phase 1, bake_ver 24). Same shape
// and the same posture as the basin table: every field checked, because a head
// row is also a gameplay instruction -- "emit this much water, here".
bool parseHeadwaterTable(const uint8_t* tableData, uint64_t len, uint32_t tileSize,
                         std::vector<HeadEntry>& out) {
    if (len < kHeadwaterTableHeaderBytes) return false;
    ByteReader r(tableData, static_cast<size_t>(len));
    uint16_t version, entryBytes;
    uint32_t count;
    if (!r.u16(version) || version != kHeadwaterTableVersion) return false;
    if (!r.u16(entryBytes) || entryBytes != kHeadwaterEntryBytes) return false;
    if (!r.u32(count)) return false;
    if (len != kHeadwaterTableHeaderBytes + static_cast<uint64_t>(count) * kHeadwaterEntryBytes) {
        return false;
    }

    out.clear();
    out.reserve(count);
    int64_t prev = -1;
    for (uint32_t i = 0; i < count; ++i) {
        HeadEntry h;
        if (!r.u16(h.px) || !r.u16(h.py)) return false;
        if (!r.u32(h.qM3PerYear)) return false;
        if (h.px >= tileSize || h.py >= tileSize) return false;
        // STRICTLY ascending by (y, x). Free for the producer (it walks a
        // raster mask) and load-bearing for the consumer: a repeated point is
        // one faucet emitted twice, i.e. twice the water in one place, which
        // no downstream conservation check could attribute.
        const int64_t key = (static_cast<int64_t>(h.py) << 17) | h.px;
        if (key <= prev) return false;
        prev = key;
        out.push_back(h);
    }
    return r.atEnd();
}

} // namespace

bool readFineSectionTable(const uint8_t* head, size_t headLen, uint16_t& flags,
                          uint64_t& fileSize, std::vector<FineSectionEntry>& out) {
    out.clear();
    if (head == nullptr || headLen < kFineHeaderBytes) return false;
    ByteReader r(head, headLen);
    uint8_t m0, m1, m2, m3;
    if (!r.u8(m0) || !r.u8(m1) || !r.u8(m2) || !r.u8(m3)) return false;
    if (m0 != 'V' || m1 != 'X' || m2 != 'T' || m3 != 'L') return false;
    uint16_t version;
    if (!r.u16(version) || version != kFineFormatVersion) return false;

    // Skip to the two fields this peek actually needs. Offsets are spelled out
    // rather than "skip 30" so a header layout change breaks here loudly
    // instead of silently reading `flags` out of `bake_ver`.
    if (!r.skip(8 + 4 + 4 + 1 + 2)) return false;   // seed, x, y, scale, size
    if (!r.skip(1 + 1 + 1 + 1 + 2)) return false;   // blockLog2, predictor, quant, codec, bakeVer
    if (!r.u16(flags)) return false;
    if (!r.skip(4 + 1 + 3)) return false;           // baseOffsetMm, parentScale, reserved

    uint16_t nSections;
    if (!r.u16(nSections)) return false;

    const uint64_t tableBytes = static_cast<uint64_t>(nSections) * kFineSectionEntryBytes;
    if (kFineHeaderBytes + tableBytes > headLen) return false;  // probe too small: re-probe

    fileSize = kFineHeaderBytes + tableBytes;
    out.reserve(nSections);
    for (uint16_t i = 0; i < nSections; ++i) {
        FineSectionEntry e;
        if (!r.u32(e.id) || !r.u64(e.offset) || !r.u64(e.length)) return false;
        if (e.length > UINT64_MAX - e.offset) return false;
        fileSize = std::max(fileSize, e.offset + e.length);
        out.push_back(e);
    }
    return true;
}

// ---------------------------------------------------------------------------
// FineTileBytes -- the sparse, offset-addressed byte store behind a partial
// tile. See the header for why span() must never fabricate a byte.
// ---------------------------------------------------------------------------

bool FineTileBytes::addSegment(uint64_t offset, std::vector<uint8_t> data) {
    if (data.empty()) return true;                       // nothing to hold
    const uint64_t end = offset + data.size();
    if (end < offset) return false;                      // overflow
    if (end > fileSize_) return false;                   // past the file

    // Overlap check against everything held. Refusing rather than merging is
    // the point (see the header): two answers for one byte can only come from a
    // mis-planned fetch or a transport that served a range other than the one
    // requested, and both must be loud.
    size_t insertAt = segs_.size();
    for (size_t i = 0; i < segs_.size(); ++i) {
        if (offset < segs_[i].end() && segs_[i].offset < end) return false;
        if (segs_[i].offset > offset && i < insertAt) insertAt = i;
    }

    segs_.insert(segs_.begin() + static_cast<std::ptrdiff_t>(insertAt),
                 Segment{offset, std::move(data)});

    // Merge with the neighbour on either side when they are exactly adjacent,
    // so a coalesced plan that arrives as several requests still presents each
    // block's payload as one contiguous span.
    if (insertAt + 1 < segs_.size() && segs_[insertAt].end() == segs_[insertAt + 1].offset) {
        Segment& a = segs_[insertAt];
        Segment& b = segs_[insertAt + 1];
        a.data.insert(a.data.end(), b.data.begin(), b.data.end());
        segs_.erase(segs_.begin() + static_cast<std::ptrdiff_t>(insertAt) + 1);
    }
    if (insertAt > 0 && segs_[insertAt - 1].end() == segs_[insertAt].offset) {
        Segment& a = segs_[insertAt - 1];
        Segment& b = segs_[insertAt];
        a.data.insert(a.data.end(), b.data.begin(), b.data.end());
        segs_.erase(segs_.begin() + static_cast<std::ptrdiff_t>(insertAt));
    }
    // A store that has been added to piecemeal is no longer "the whole file" as
    // a CLAIM, even if the segments happen to cover it; isWhole() means "built
    // by whole()", and nothing depends on it being the tighter statement.
    return true;
}

const uint8_t* FineTileBytes::span(uint64_t off, uint64_t len) const {
    if (len == 0) return nullptr;             // see the header: never useful
    const uint64_t end = off + len;
    if (end < off) return nullptr;            // overflow
    for (const Segment& s : segs_) {
        if (off >= s.offset && end <= s.end()) {
            return s.data.data() + static_cast<size_t>(off - s.offset);
        }
    }
    return nullptr;                           // not fetched -- NOT zeroes
}

uint64_t FineTileBytes::residentBytes() const {
    uint64_t n = 0;
    for (const Segment& s : segs_) n += s.data.size();
    return n;
}

const char* fineErrorName(FineError e) {
    switch (e) {
    case FineError::kNone: return "none";
    case FineError::kFileUnreadable: return "file-unreadable";
    case FineError::kNotVxtl: return "not-a-vxtl";
    case FineError::kWrongVersion: return "wrong-version";
    case FineError::kBadHeader: return "bad-header";
    case FineError::kUnknownFeature: return "unknown-feature";
    case FineError::kUnknownCodec: return "unknown-codec";
    case FineError::kNoDecompressor: return "no-decompressor";
    case FineError::kBadSectionTable: return "bad-section-table";
    case FineError::kBadBlockIndex: return "bad-block-index";
    case FineError::kBadBlockCoords: return "bad-block-coords";
    case FineError::kDecompressFailed: return "decompress-failed";
    case FineError::kBadPayload: return "bad-payload";
    case FineError::kValueOutOfRange: return "value-out-of-range";
    case FineError::kBadBasinTable: return "bad-basin-table";
    case FineError::kBadHeadwaterTable: return "bad-headwater-table";
    case FineError::kBlockNotResident: return "block-not-resident";
    }
    return "unknown";
}

bool fineReadHeaderFacts(const uint8_t* head, size_t headLen, FineHeaderFacts& out) {
    out = FineHeaderFacts{};
    if (head == nullptr || headLen < kFineHeaderBytes) return false;
    ByteReader r(head, headLen);

    uint8_t m0, m1, m2, m3;
    if (!r.u8(m0) || !r.u8(m1) || !r.u8(m2) || !r.u8(m3)) return false;
    if (m0 != 'V' || m1 != 'X' || m2 != 'T' || m3 != 'L') return false;
    out.magicOk = true;
    if (!r.u16(out.formatVersion)) return false;

    // A file that is not v2 gets NOTHING further reported about it. The v2
    // fields below are positional and a v1 file's bytes at those offsets mean
    // something else entirely -- printing them would put a fabricated
    // `bake_ver` into the very message someone is reading to decide whether
    // their build is too old.
    if (out.formatVersion != kFineFormatVersion) return true;

    // Skips spelled out field by field for the same reason
    // readFineSectionTable spells them out: a §3 layout change must break here
    // loudly rather than quietly report `flags` as `bake_ver`.
    if (!r.skip(8 + 4 + 4 + 1 + 2)) return true;  // seed, x, y, scale, size
    if (!r.skip(1 + 1 + 1)) return true;          // blockLog2, predictor, quant
    if (!r.u8(out.codec)) return true;
    if (!r.u16(out.bakeVer)) return true;
    if (!r.u16(out.flags)) return true;
    out.v2Fields = true;
    return true;
}

namespace {

std::string hex16(uint16_t v) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s = "0x";
    for (int shift = 12; shift >= 0; shift -= 4) {
        s.push_back(kDigits[(v >> shift) & 0xf]);
    }
    return s;
}

} // namespace

std::string fineDescribeRejection(FineError e, const FineHeaderFacts& facts) {
    // WHAT THE FILE CLAIMS TO BE. Half of every version-skew message, and the
    // half a corrupt-file message can also carry harmlessly.
    std::string fileSays;
    if (!facts.magicOk) {
        fileSays = " The file has no VXTL magic.";
    } else {
        fileSays = " File: .vxtl v" + std::to_string(facts.formatVersion);
        if (facts.v2Fields) {
            fileSays += ", bake_ver " + std::to_string(facts.bakeVer) + ", flags " +
                        hex16(facts.flags) + ", codec " + std::to_string(unsigned(facts.codec));
        }
        fileSays += ".";
    }
    // WHAT THIS BUILD CAN READ. The other half, and the one that was missing:
    // without it "bad-header" is a statement about the tile with no way to
    // read it as a statement about the reader.
    // "up to N (last format-affecting bake)" rather than a bare "up to N",
    // because kFineMaxKnownBakeVer deliberately LAGS the bake pipeline's
    // counter: bake_ver bumps that change no section and no flag need no
    // reader change, and a message that read them as "you are behind" would
    // start blaming the build for every unrelated refusal.
    const std::string buildSays = " This build: .vxtl v" + std::to_string(kFineFormatVersion) +
                                  ", known flags " + hex16(kFineFlagsKnown) + ", bake_ver up to " +
                                  std::to_string(kFineMaxKnownBakeVer) +
                                  " (last format-affecting bake).";

    switch (e) {
    case FineError::kNone:
        return "no error.";
    case FineError::kUnknownFeature:
        return "REFUSED BY THIS BUILD, NOT BY THE FILE: it declares feature flag bit(s) " +
               hex16(facts.unknownFlagBits()) + " that this build does not implement." + fileSays +
               buildSays +
               " A tile NEWER than its reader looks exactly like this. Rebuild the game module "
               "against the current voxel-core before suspecting the tile or re-baking it.";
    case FineError::kWrongVersion:
        return "the .vxtl format version is not the one this build reads." + fileSays + buildSays +
               " If the file's version is the HIGHER number, this build is too old.";
    case FineError::kUnknownCodec:
        return "the `codec` byte names a compression this build does not know." + fileSays +
               buildSays + " A codec added after this build was made looks exactly like this.";
    case FineError::kNoDecompressor:
        return "the file is compressed and no decompressor was injected into this build -- a HOST "
               "WIRING bug here, not a problem with the tile." +
               fileSays;
    case FineError::kFileUnreadable:
        return "a read against the file failed: it may be locked, still being written, or "
               "truncated under us. Transient -- worth retrying." +
               fileSays;
    case FineError::kBlockNotResident:
        return "the bytes for this part of the tile were never fetched. INCOMPLETE, not corrupt -- "
               "fetch more and retry; do not discard." +
               fileSays;
    case FineError::kNotVxtl:
        return "not a .vxtl at all: no VXTL magic, or too short to hold a header.";
    case FineError::kBadHeader:
    case FineError::kBadSectionTable:
    case FineError::kBadBlockIndex:
    case FineError::kBadBlockCoords:
    case FineError::kDecompressFailed:
    case FineError::kBadPayload:
    case FineError::kValueOutOfRange:
    case FineError::kBadBasinTable:
    case FineError::kBadHeadwaterTable:
        break;
    }
    // The genuinely-malformed codes. They still carry both version numbers,
    // because "is my build too old" is the first question anyone asks of any
    // refusal and answering it here is what keeps the next reason code from
    // becoming the next bad-header.
    return std::string("the file is structurally malformed (") + fineErrorName(e) + ")." +
           fileSays + buildSays;
}

std::optional<FineTile> FineTile::parse(const uint8_t* data, size_t size,
                                        const FineDecompressor& decompressor, FineError* err) {
    return FineTile::parse(std::vector<uint8_t>(data, data + size), decompressor, err);
}

std::optional<FineTile> FineTile::parse(std::vector<uint8_t> bytes,
                                        const FineDecompressor& decompressor, FineError* err) {
    return FineTile::parsePartial(FineTileBytes::whole(std::move(bytes)), decompressor, err);
}

std::optional<FineTile> FineTile::parsePartial(FineTileBytes bytes,
                                               const FineDecompressor& decompressor,
                                               FineError* err) {
    if (err) *err = FineError::kNone;
    // Every failing return below funnels through this, so "returned nullopt"
    // and "recorded a reason" can never drift apart.
    const auto reject = [err](FineError e) -> std::optional<FineTile> {
        if (err) *err = e;
        return std::nullopt;
    };

    const uint64_t fileSize = bytes.fileSize();

    // THE FIXED HEADER MUST BE HELD. On a whole-file load this is trivially
    // true; on a sliced one it is the first thing the head probe buys. Note the
    // reason code: a header that was never fetched is kBlockNotResident (fetch
    // more), NOT kBadHeader (throw the tile away) -- the two call for opposite
    // responses and a client that confused them would either retry forever on
    // corrupt bytes or discard a tile it merely had not finished reading.
    const uint8_t* head = bytes.span(0, kFineHeaderBytes);
    if (head == nullptr) {
        return reject(fileSize < kFineHeaderBytes ? FineError::kNotVxtl
                                                  : FineError::kBlockNotResident);
    }
    ByteReader r(head, kFineHeaderBytes);

    uint8_t m0, m1, m2, m3;
    if (!r.u8(m0) || !r.u8(m1) || !r.u8(m2) || !r.u8(m3)) return reject(FineError::kNotVxtl);
    if (m0 != 'V' || m1 != 'X' || m2 != 'T' || m3 != 'L') return reject(FineError::kNotVxtl);

    uint16_t version;
    if (!r.u16(version) || version != kFineFormatVersion) return reject(FineError::kWrongVersion);

    FineTileHeader h;
    if (!r.u64(h.seed)) return reject(FineError::kBadHeader);
    if (!r.i32(h.x) || !r.i32(h.y)) return reject(FineError::kBadHeader);
    if (!r.u8(h.scale) || h.scale != kFineTileScale) return reject(FineError::kBadHeader);
    if (!r.u16(h.size)) return reject(FineError::kBadHeader);
    // §3 gives 8192 as the fine grid edge and that is what production bakes,
    // but `size` is a header FIELD and the committed conformance fixture is a
    // deliberately small 512-edge tile. Validating it structurally -- a power
    // of two, no larger than the production edge -- means the fixture goes
    // through exactly the same code path a real tile does, which is the only
    // way it proves anything.
    if (h.size < 16 || h.size > kFineTileSize) return reject(FineError::kBadHeader);
    if ((h.size & static_cast<uint16_t>(h.size - 1)) != 0) return reject(FineError::kBadHeader);

    // --- v2 extension (the first 25 bytes above are v1-positional) ---
    if (!r.u8(h.blockLog2)) return reject(FineError::kBadHeader);
    // §3 documents block_log2 = 8. The field exists so it can vary, so the
    // check is structural rather than a hardcoded 8: the block must tile the
    // grid exactly, and must stay small enough that one decoded block is a
    // sane allocation (a CONSTANT block costs zero file bytes, so without an
    // upper bound a tiny file could ask for an enormous buffer).
    if (h.blockLog2 < 1 || h.blockLog2 > 12) return reject(FineError::kBadHeader);
    if ((h.size >> h.blockLog2) << h.blockLog2 != h.size) return reject(FineError::kBadHeader);

    if (!r.u8(h.predictor) || h.predictor != kPredMed) return reject(FineError::kBadHeader);
    if (!r.u8(h.quant) || fineQuantMm(h.quant) == 0) return reject(FineError::kBadHeader);
    if (!r.u8(h.codec)) return reject(FineError::kBadHeader);
    // Two different refusals, deliberately distinguishable (see FineError):
    // an unknown codec byte is bytes this build does not understand, while
    // CODEC_ZSTD with nothing injected is a HOST WIRING mistake. Either way the
    // tile is refused whole -- it is never loaded and left to decode blocks
    // into zeros, because silently flat terrain under a client that thinks it
    // has the fine tier is a desync, not a visual glitch.
    if (!fineCodecKnown(h.codec)) return reject(FineError::kUnknownCodec);
    const bool compressed = fineCodecNeedsDecompressor(h.codec);
    if (compressed && !decompressor.valid()) return reject(FineError::kNoDecompressor);
    if (!r.u16(h.bakeVer)) return reject(FineError::kBadHeader);
    if (!r.u16(h.flags)) return reject(FineError::kBadHeader);
    // STILL REFUSED WHOLE -- only the reason code changes. An undefined flag
    // bit is the format's own "this file carries something you do not
    // implement" signal, and by far its likeliest cause is a reader older than
    // its tiles rather than damaged bytes. kBadHeader said the opposite and
    // cost an evening saying it; see FineError::kUnknownFeature.
    if ((h.flags & ~kFineFlagsKnown) != 0) return reject(FineError::kUnknownFeature);
    if (!r.i32(h.baseOffsetMm)) return reject(FineError::kBadHeader);
    // §3: absolute only.
    if (!r.u8(h.parentScale) || h.parentScale != 0) return reject(FineError::kBadHeader);
    for (int i = 0; i < 3; ++i) {
        uint8_t reserved;
        if (!r.u8(reserved) || reserved != 0) return reject(FineError::kBadHeader);
    }

    uint16_t nSections;
    if (!r.u16(nSections)) return reject(FineError::kBadHeader);

    const uint64_t tableBytes = static_cast<uint64_t>(nSections) * kFineSectionEntryBytes;
    const uint64_t tableEnd = static_cast<uint64_t>(kFineHeaderBytes) + tableBytes;
    if (tableEnd > fileSize) return reject(FineError::kBadSectionTable);

    // The table is its own read because the header reader above is bounded to
    // the 43 header bytes -- on a partial tile there is no single buffer that
    // spans both, and pretending otherwise is how a sliced parse reads a
    // section entry out of whatever happened to follow in memory.
    const uint8_t* tableData = tableBytes == 0 ? head : bytes.span(kFineHeaderBytes, tableBytes);
    if (tableData == nullptr) return reject(FineError::kBlockNotResident);
    ByteReader table(tableData, static_cast<size_t>(tableBytes));

    SectionRef elevIndexSec, elevDataSec, flowIndexSec, flowDataSec, basinSec, headSec;
    SectionRef waterIndexSec, waterDataSec;
    struct Extent {
        uint64_t begin, end;
    };
    std::vector<Extent> extents;
    std::vector<uint32_t> seenIds;
    uint64_t coveredEnd = tableEnd;

    for (uint16_t i = 0; i < nSections; ++i) {
        uint32_t id;
        uint64_t off, len;
        if (!table.u32(id) || !table.u64(off) || !table.u64(len)) {
            return reject(FineError::kBadSectionTable);
        }

        // In bounds, no overflow, and never on top of the header/table.
        if (off > fileSize) return reject(FineError::kBadSectionTable);
        if (len > fileSize - off) return reject(FineError::kBadSectionTable);
        if (len > 0 && off < tableEnd) return reject(FineError::kBadSectionTable);

        if (std::find(seenIds.begin(), seenIds.end(), id) != seenIds.end()) {
            return reject(FineError::kBadSectionTable);
        }
        seenIds.push_back(id);

        if (len > 0) {
            for (const Extent& e : extents) {
                if (off < e.end && e.begin < off + len) { // overlap
                    return reject(FineError::kBadSectionTable);
                }
            }
            extents.push_back({off, off + len});
            coveredEnd = std::max(coveredEnd, off + len);
        }

        const SectionRef ref{off, len, true};
        switch (id) {
        case kSectionElevIndex: elevIndexSec = ref; break;
        case kSectionElevData: elevDataSec = ref; break;
        case kSectionFlowIndex: flowIndexSec = ref; break;
        case kSectionFlowData: flowDataSec = ref; break;
        case kSectionBasinTable: basinSec = ref; break;
        case kSectionWaterIndex: waterIndexSec = ref; break;
        case kSectionWaterData: waterDataSec = ref; break;
        case kSectionHeadwaters: headSec = ref; break;
        default: break; // see (b): unknown ids are ignored but still bounded
        }
    }

    // The v1 parser rejects trailing bytes; the section table is what makes
    // "trailing" meaningful here, so: nothing may live past the last section.
    // Interior gaps are allowed (an encoder may align sections), because
    // offsets are explicit and a gap cannot be misread as data.
    if (coveredEnd != fileSize) return reject(FineError::kBadSectionTable);

    if (!elevIndexSec.present || !elevDataSec.present) return reject(FineError::kBadSectionTable);
    const bool wantFlow = (h.flags & kFineFlagFlowPresent) != 0;
    if (wantFlow != (flowIndexSec.present && flowDataSec.present)) {
        return reject(FineError::kBadSectionTable);
    }
    if (!wantFlow && (flowIndexSec.present || flowDataSec.present)) {
        return reject(FineError::kBadSectionTable);
    }
    // Flag and section must agree in BOTH directions, same as flow: a table
    // with the flag clear is bytes this decoder would ignore, and a flag with
    // no table is a client that would silently place no water.
    const bool wantBasins = (h.flags & kFineFlagBasinsPresent) != 0;
    if (wantBasins != basinSec.present) return reject(FineError::kBadSectionTable);
    // Same both-directions agreement for the water plane. The "sections
    // present, flag clear" half is the one that matters most here: those bytes
    // would be silently ignored and every river in the tile would vanish
    // without a single error being raised anywhere.
    const bool wantWater = (h.flags & kFineFlagWaterPresent) != 0;
    if (wantWater != (waterIndexSec.present && waterDataSec.present)) {
        return reject(FineError::kBadSectionTable);
    }
    if (!wantWater && (waterIndexSec.present || waterDataSec.present)) {
        return reject(FineError::kBadSectionTable);
    }
    // Fourth section, same both-directions rule, same reason: a head table
    // whose flag is clear would be silently ignored and every faucet in the
    // tile would go missing without a word.
    const bool wantHeads = (h.flags & kFineFlagHeadsPresent) != 0;
    if (wantHeads != headSec.present) return reject(FineError::kBadSectionTable);

    // Resolves one PREAMBLE section's bytes. The distinction it carries is the
    // one this whole partial path exists for: nullptr with the section declared
    // present means "not fetched yet" (kBlockNotResident, go and get it), never
    // "corrupt" and never an empty buffer that would parse as zero entries.
    const auto preamble = [&bytes](const SectionRef& s) -> const uint8_t* {
        return bytes.span(s.offset, s.length);
    };

    // A DECLARED-BUT-UNFETCHED preamble section is not an error -- it is the
    // normal state of a client that asked for ground only and never wanted the
    // flow index. What must not happen is for it to be mistaken for an EMPTY
    // one: hasBasins() with an empty table means "surveyed, no basins here",
    // and an unfetched table must not read as that. Hence a residency flag per
    // section rather than an empty container standing in for both.
    //
    // ELEV_INDEX is the one exception and is required: it is 20 KB, it is
    // always inside the head probe (it is the first section), and every other
    // accessor on this class is meaningless without it.
    std::vector<BasinEntry> basins;
    bool basinsResident = false;
    if (wantBasins) {
        const uint8_t* basinData = preamble(basinSec);
        if (basinData != nullptr) {
            if (!parseBasinTable(basinData, basinSec.length, h.size, h.x, h.y, basins)) {
                return reject(FineError::kBadBasinTable);
            }
            basinsResident = true;
        }
    }

    std::vector<HeadEntry> heads;
    bool headsResident = false;
    if (wantHeads) {
        const uint8_t* headData = preamble(headSec);
        if (headData != nullptr) {
            if (!parseHeadwaterTable(headData, headSec.length, h.size, heads)) {
                return reject(FineError::kBadHeadwaterTable);
            }
            headsResident = true;
        }
    }

    FineTile tile;
    tile.h_ = h;
    tile.dec_ = decompressor;
    tile.elevDataOff_ = elevDataSec.offset;
    tile.elevDataLen_ = elevDataSec.length;
    tile.flowDataOff_ = flowDataSec.offset;
    tile.flowDataLen_ = flowDataSec.length;
    tile.waterDataOff_ = waterDataSec.offset;
    tile.waterDataLen_ = waterDataSec.length;

    const uint32_t perAxis = static_cast<uint32_t>(h.size) >> h.blockLog2;
    const uint32_t blocks = perAxis * perAxis;
    const uint32_t dim = 1u << h.blockLog2;
    const uint32_t blockPixels = dim * dim;

    const uint8_t* elevIndexData = preamble(elevIndexSec);
    if (elevIndexData == nullptr) return reject(FineError::kBlockNotResident);
    if (!parseBlockIndex(elevIndexData, elevIndexSec.length, blocks, blockPixels,
                         elevDataSec.length, 2, compressed, tile.elevIndex_)) {
        return reject(FineError::kBadBlockIndex);
    }
    if (wantFlow) {
        const uint8_t* flowIndexData = preamble(flowIndexSec);
        if (flowIndexData != nullptr) {
            if (!parseBlockIndex(flowIndexData, flowIndexSec.length, blocks, blockPixels,
                                 flowDataSec.length, 1, compressed, tile.flowIndex_)) {
                return reject(FineError::kBadBlockIndex);
            }
        }
    }

    // Element width 2, exactly like elevation: the water plane IS an elevation
    // plane, on the same datum, which is what lets it share every byte of this
    // machinery instead of growing a second one to keep in step.
    if (wantWater) {
        const uint8_t* waterIndexData = preamble(waterIndexSec);
        if (waterIndexData != nullptr) {
            if (!parseBlockIndex(waterIndexData, waterIndexSec.length, blocks, blockPixels,
                                 waterDataSec.length, 2, compressed, tile.waterIndex_)) {
                return reject(FineError::kBadBlockIndex);
            }
        }
    }

    tile.basinsResident_ = basinsResident;
    tile.basins_ = std::move(basins);
    tile.headsResident_ = headsResident;
    tile.heads_ = std::move(heads);
    tile.bytes_ = std::move(bytes);
    return tile;
}

// ---------------------------------------------------------------------------
// Per-block residency, and the decoders that respect it.
//
// ONE function decides which bytes a block needs, and both the residency test
// and the decoder call it. That is deliberate: if the gate and the reader
// computed the span separately, they could disagree by an entry -- the gate
// would say "resident", the decoder would find nothing, and the caller would be
// holding a plausible-looking block of zeroes. Sea level, silently.
// ---------------------------------------------------------------------------

bool FineTile::blockFileSpan(const std::vector<FineBlockEntry>& index, uint64_t dataOff,
                             uint32_t bx, uint32_t by, uint64_t& off, uint64_t& len) const {
    const uint32_t perAxis = blocksPerAxis();
    if (bx >= perAxis || by >= perAxis) return false;
    const size_t i = static_cast<size_t>(by) * perAxis + bx;
    if (i >= index.size()) return false;      // plane not carried by this tile
    const FineBlockEntry& e = index[i];
    if (e.mode == kBlockConstant) {
        // Trap #1 (docs/tile-slicing-2026-08-04.md §2): a CONSTANT block's
        // (offset=0, comp_len=0) is NOT a range. Byte 0 of the data section
        // belongs to some other block. It owns nothing and needs nothing.
        off = 0;
        len = 0;
        return true;
    }
    off = dataOff + e.offset;
    len = e.compLen;
    return true;
}

bool FineTile::elevBlockResident(uint32_t bx, uint32_t by) const {
    uint64_t off = 0, len = 0;
    if (!blockFileSpan(elevIndex_, elevDataOff_, bx, by, off, len)) return false;
    return len == 0 || bytes_.covers(off, len);
}

bool FineTile::flowBlockResident(uint32_t bx, uint32_t by) const {
    if (!hasFlow()) return false;
    uint64_t off = 0, len = 0;
    if (!blockFileSpan(flowIndex_, flowDataOff_, bx, by, off, len)) return false;
    return len == 0 || bytes_.covers(off, len);
}

bool FineTile::waterBlockResident(uint32_t bx, uint32_t by) const {
    if (!hasWater()) return false;
    uint64_t off = 0, len = 0;
    if (!blockFileSpan(waterIndex_, waterDataOff_, bx, by, off, len)) return false;
    return len == 0 || bytes_.covers(off, len);
}

uint32_t FineTile::residentElevBlocks() const {
    const uint32_t perAxis = blocksPerAxis();
    uint32_t n = 0;
    for (uint32_t by = 0; by < perAxis; ++by)
        for (uint32_t bx = 0; bx < perAxis; ++bx)
            if (elevBlockResident(bx, by)) ++n;
    return n;
}

uint32_t FineTile::residentWaterBlocks() const {
    if (!hasWater()) return 0;
    const uint32_t perAxis = blocksPerAxis();
    uint32_t n = 0;
    for (uint32_t by = 0; by < perAxis; ++by)
        for (uint32_t bx = 0; bx < perAxis; ++bx)
            if (waterBlockResident(bx, by)) ++n;
    return n;
}

bool FineTile::addFetchedBytes(uint64_t fileOffset, std::vector<uint8_t> data) {
    return bytes_.addSegment(fileOffset, std::move(data));
}

namespace {

// Stands in for the data-section base of a CONSTANT block. decodeBlockPayload
// takes the CONSTANT branch before it looks at `data` at all, so this is never
// dereferenced -- it exists only so "resident" and "non-null" can stay the same
// test for every mode. Returning nullptr for a CONSTANT block instead would
// make the cheapest and commonest case on the water plane (72-87% of it) look
// not-resident and block a client that in fact holds everything it needs.
const uint8_t kConstantBlockNoData = 0;

// The data-section base a decoder needs, or nullptr when the block's bytes were
// never fetched. Returns the base the block's `offset` is relative to, not the
// block's own start, so decodeBlockPayload's existing "e.offset into data"
// contract is unchanged.
const uint8_t* planeDataFor(const FineTileBytes& bytes, const FineBlockEntry& e,
                            uint64_t dataOff, uint64_t dataLen) {
    if (e.mode == kBlockConstant) return &kConstantBlockNoData;
    if (e.compLen == 0) return nullptr;
    if (e.offset > dataLen || e.compLen > dataLen - e.offset) return nullptr;
    const uint8_t* p = bytes.span(dataOff + e.offset, e.compLen);
    if (p == nullptr) return nullptr;
    return p - static_cast<size_t>(e.offset);
}

} // namespace

bool FineTile::decodeElevBlock(uint32_t bx, uint32_t by, std::vector<int16_t>& out,
                               FineError* err) const {
    if (err) *err = FineError::kNone;
    const uint32_t perAxis = blocksPerAxis();
    if (bx >= perAxis || by >= perAxis) return fail(err, FineError::kBadBlockCoords);
    const uint32_t dim = blockDim();
    const FineBlockEntry& e = elevIndex_[by * perAxis + bx];
    const uint8_t* base = planeDataFor(bytes_, e, elevDataOff_, elevDataLen_);
    if (base == nullptr) {
        // NOT ZEROES, and this is the single most important branch in the file.
        // `out` is left untouched and false is returned with kBlockNotResident,
        // because filling it with 0 would put this block's terrain at SEA LEVEL
        // -- a different world from the one every other client computes, and one
        // that would surface as a rendering bug somewhere far from here.
        return fail(err, FineError::kBlockNotResident);
    }
    out.assign(blockPixelCount(), 0);
    return decodeBlockPayload<int16_t>(e, base, static_cast<size_t>(elevDataLen_), dim, -32768,
                                       32767, dec_, fineCodecNeedsDecompressor(h_.codec),
                                       out.data(), err);
}

bool FineTile::decodeFlowBlock(uint32_t bx, uint32_t by, std::vector<uint8_t>& out,
                               FineError* err) const {
    if (err) *err = FineError::kNone;
    if (!hasFlow()) return fail(err, FineError::kBadBlockCoords);
    const uint32_t perAxis = blocksPerAxis();
    if (bx >= perAxis || by >= perAxis) return fail(err, FineError::kBadBlockCoords);
    // The tile declares a flow plane but this client never fetched its index,
    // so there is no way to know where the block is. Not-resident, not corrupt.
    // (Also the bounds check that keeps the indexing below defined on a sliced
    // tile, where flowIndex_ is legitimately empty.)
    if (!flowIndexResident()) return fail(err, FineError::kBlockNotResident);
    const uint32_t dim = blockDim();
    const FineBlockEntry& e = flowIndex_[by * perAxis + bx];
    const uint8_t* base = planeDataFor(bytes_, e, flowDataOff_, flowDataLen_);
    if (base == nullptr) return fail(err, FineError::kBlockNotResident);
    out.assign(blockPixelCount(), 0);
    return decodeBlockPayload<uint8_t>(e, base, static_cast<size_t>(flowDataLen_), dim, 0, 255,
                                       dec_, fineCodecNeedsDecompressor(h_.codec), out.data(),
                                       err);
}

bool FineTile::decodeWaterBlock(uint32_t bx, uint32_t by, std::vector<int16_t>& out,
                               FineError* err) const {
    if (err) *err = FineError::kNone;
    if (!hasWater()) return fail(err, FineError::kBadBlockCoords);
    const uint32_t perAxis = blocksPerAxis();
    if (bx >= perAxis || by >= perAxis) return fail(err, FineError::kBadBlockCoords);
    // Index not fetched: not-resident, NOT dry. See decodeWaterBlock's payload
    // branch below -- the same argument, one level earlier.
    if (!waterIndexResident()) return fail(err, FineError::kBlockNotResident);
    const uint32_t dim = blockDim();
    const FineBlockEntry& e = waterIndex_[by * perAxis + bx];
    const uint8_t* base = planeDataFor(bytes_, e, waterDataOff_, waterDataLen_);
    if (base == nullptr) {
        // Distinct from the dry sentinel below ON PURPOSE. "Not fetched" and
        // "no water here" are different facts: filling with kWaterDryDepth and
        // returning true would make an unfetched reach read as a dry riverbed,
        // which is exactly the class of silent loss the water plane's own
        // present/absent flag exists to prevent.
        return fail(err, FineError::kBlockNotResident);
    }
    // Filled with the DRY sentinel, not 0. 0 is a legitimate stored DEPTH
    // (water exactly at its own bed, which is what a reach's shallow edge
    // quantises to), so a block that failed to decode must read as the
    // sentinel or it becomes a sheet of zero-depth water over the whole block.
    out.assign(blockPixelCount(), kWaterDryDepth);
    return decodeBlockPayload<int16_t>(e, base, static_cast<size_t>(waterDataLen_), dim, -32768,
                                       32767, dec_, fineCodecNeedsDecompressor(h_.codec),
                                       out.data(), err);
}

bool FineTile::controlPointAt(uint32_t lx, uint32_t ly, int16_t& cp, FineError* err) const {
    if (err) *err = FineError::kNone;
    if (lx >= h_.size || ly >= h_.size) return fail(err, FineError::kBadBlockCoords);
    const uint32_t dim = blockDim();
    std::vector<int16_t> block;
    if (!decodeElevBlock(lx >> h_.blockLog2, ly >> h_.blockLog2, block, err)) return false;
    cp = block[(ly & (dim - 1)) * dim + (lx & (dim - 1))];
    return true;
}

bool FineTileSampler::loadTile(FineTile tile) {
    if (tile.seed() != seed_) return false;
    if (tileSize_ != 0 && tile.size() != tileSize_) return false;
    const uint64_t key = tileKey(tile.tileX(), tile.tileY());
    tileSize_ = tile.size();
    Resident r;
    r.tile = std::move(tile);
    tiles_.insert_or_assign(key, std::move(r));
    return true;
}

bool FineTileSampler::loadTile(const std::vector<uint8_t>& bytes, FineError* err) {
    std::optional<FineTile> parsed = FineTile::parse(bytes.data(), bytes.size(), dec_, err);
    if (!parsed) return false;
    return loadTile(std::move(*parsed));
}

bool FineTileSampler::loadTileFile(const std::filesystem::path& path, FineError* err) {
    if (err) *err = FineError::kNone;
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    if (!bytes) return fail(err, FineError::kFileUnreadable);
    return loadTile(*bytes, err);
}

const FineTile* FineTileSampler::findTile(int32_t tx, int32_t ty) const {
    auto it = tiles_.find(tileKey(tx, ty));
    return it == tiles_.end() ? nullptr : &it->second.tile;
}

bool FineTileSampler::unloadTile(int32_t tx, int32_t ty) {
    const bool erased = tiles_.erase(tileKey(tx, ty)) > 0;
    if (erased && tiles_.empty()) {
        tileSize_ = 0; // no tile left to justify the stride; see the header comment
    }
    return erased;
}

size_t FineTileSampler::residentBlockCount() const {
    size_t n = 0;
    for (const auto& kv : tiles_) n += kv.second.blocks.size();
    return n;
}

const std::vector<int16_t>* FineTileSampler::blockFor(int64_t px, int64_t py,
                                                      const FineTile*& tile, uint32_t& lx,
                                                      uint32_t& ly) {
    tile = nullptr;
    if (tileSize_ == 0) { // nothing loaded: every pixel is a miss
        missingTileQueries.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    const int64_t sz = static_cast<int64_t>(tileSize_);
    const int32_t tx = static_cast<int32_t>(floorDiv(px, sz));
    const int32_t ty = static_cast<int32_t>(floorDiv(py, sz));
    auto it = tiles_.find(tileKey(tx, ty));
    if (it == tiles_.end()) {
        missingTileQueries.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    Resident& res = it->second;
    const uint32_t fx = static_cast<uint32_t>(floorMod(px, sz));
    const uint32_t fy = static_cast<uint32_t>(floorMod(py, sz));
    const uint8_t log2 = res.tile.header().blockLog2;
    const uint32_t dim = res.tile.blockDim();
    const uint32_t perAxis = res.tile.blocksPerAxis();
    const uint32_t bx = fx >> log2, by = fy >> log2;
    lx = fx & (dim - 1);
    ly = fy & (dim - 1);

    const uint32_t key = by * perAxis + bx;
    auto bit = res.blocks.find(key);
    if (bit == res.blocks.end()) {
        std::vector<int16_t> decoded;
        FineError err = FineError::kNone;
        if (!res.tile.decodeElevBlock(bx, by, decoded, &err)) {
            // TWO COUNTERS, because they mean opposite things and the response
            // to each is opposite. kBlockNotResident: this client holds the tile
            // but not this block's bytes -- fetch them, the data is fine.
            // Anything else: the bytes are here and they are wrong -- discard
            // the tile, re-fetching will not help. Flattening the two into
            // blockDecodeFailures would make a sliced client look permanently
            // corrupt on every un-fetched block.
            if (err == FineError::kBlockNotResident) {
                notResidentBlockQueries.fetch_add(1, std::memory_order_relaxed);
            } else {
                blockDecodeFailures.fetch_add(1, std::memory_order_relaxed);
            }
            return nullptr;
        }
        bit = res.blocks.emplace(key, std::move(decoded)).first;
    }
    tile = &res.tile;
    return &bit->second;
}

bool FineTileSampler::blockDecoded(int64_t px, int64_t py) const {
    if (tileSize_ == 0) return false;
    const int64_t sz = static_cast<int64_t>(tileSize_);
    auto it = tiles_.find(tileKey(static_cast<int32_t>(floorDiv(px, sz)),
                                  static_cast<int32_t>(floorDiv(py, sz))));
    if (it == tiles_.end()) return false;
    const Resident& res = it->second;
    const uint32_t fx = static_cast<uint32_t>(floorMod(px, sz));
    const uint32_t fy = static_cast<uint32_t>(floorMod(py, sz));
    const uint8_t log2 = res.tile.header().blockLog2;
    const uint32_t perAxis = res.tile.blocksPerAxis();
    return res.blocks.find((fy >> log2) * perAxis + (fx >> log2)) != res.blocks.end();
}

bool FineTileSampler::blockBytesResident(int64_t px, int64_t py) const {
    if (tileSize_ == 0) return false;
    const int64_t sz = static_cast<int64_t>(tileSize_);
    auto it = tiles_.find(tileKey(static_cast<int32_t>(floorDiv(px, sz)),
                                  static_cast<int32_t>(floorDiv(py, sz))));
    if (it == tiles_.end()) return false;
    const FineTile& t = it->second.tile;
    const uint8_t log2 = t.header().blockLog2;
    return t.elevBlockResident(static_cast<uint32_t>(floorMod(px, sz)) >> log2,
                               static_cast<uint32_t>(floorMod(py, sz)) >> log2);
}

uint64_t FineTileSampler::residentFileBytes() const {
    uint64_t n = 0;
    for (const auto& kv : tiles_) n += kv.second.tile.residentFileBytes();
    return n;
}

uint64_t FineTileSampler::decodedBlockBytes() const {
    uint64_t n = 0;
    for (const auto& kv : tiles_) {
        for (const auto& b : kv.second.blocks) {
            n += static_cast<uint64_t>(b.second.size()) * sizeof(int16_t);
        }
    }
    return n;
}

bool FineTileSampler::loadTilePartial(FineTileBytes bytes, FineError* err) {
    std::optional<FineTile> parsed = FineTile::parsePartial(std::move(bytes), dec_, err);
    if (!parsed) return false;
    return loadTile(std::move(*parsed));
}

bool FineTileSampler::addTileBytes(int32_t tx, int32_t ty, uint64_t fileOffset,
                                   std::vector<uint8_t> data) {
    auto it = tiles_.find(tileKey(tx, ty));
    if (it == tiles_.end()) return false;
    return it->second.tile.addFetchedBytes(fileOffset, std::move(data));
}

bool FineTileSampler::controlPointAt(int64_t px, int64_t py, int16_t& cp) {
    const FineTile* t = nullptr;
    uint32_t lx, ly;
    const std::vector<int16_t>* block = blockFor(px, py, t, lx, ly);
    if (!block) return false;
    cp = (*block)[ly * t->blockDim() + lx];
    return true;
}

int32_t FineTileSampler::elevationMm(int64_t px, int64_t py) {
    const FineTile* t = nullptr;
    uint32_t lx, ly;
    const std::vector<int16_t>* block = blockFor(px, py, t, lx, ly);
    if (!block) return kSeaLevelMm; // missing block == open ocean; see TileGridSampler
    return t->elevationMmFromCp((*block)[ly * t->blockDim() + lx]);
}

ClimateSample FineTileSampler::climate(int64_t px, int64_t py) {
    // The fine tier carries no climate plane (§1: elevation, plus the optional
    // §6 flow byte). Delegate when a coarse sampler was supplied; otherwise
    // answer the same documented bland default a missing tile gets, so a
    // caller that forgot to wire climate up gets temperate nothing-in-
    // particular rather than an accidental biome.
    //
    // The delegate is addressed in ITS OWN pixel units, via world mm and
    // floorDiv -- not by a hardcoded /16. Truncating here would fold pixels
    // -15..-1 onto coarse pixel 0 and silently mirror climate across the
    // origin, which is exactly the aliasing TileGridSampler's floorDiv routing
    // exists to avoid.
    if (!climate_) return ClimateSample{};
    const int64_t fineMm = static_cast<int64_t>(pixelSizeMm());
    const int64_t coarseMm = static_cast<int64_t>(climate_->pixelSizeMm());
    if (coarseMm <= 0) return ClimateSample{};
    return climate_->climate(floorDiv(px * fineMm, coarseMm), floorDiv(py * fineMm, coarseMm));
}

bool FineTileSampler::prewarm(int64_t px0, int64_t py0, int64_t px1, int64_t py1) {
    if (px1 < px0 || py1 < py0) return false;
    if (tiles_.empty()) {
        missingTileQueries.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Step by the smallest resident block edge. A stride below some tile's
    // block size only costs a redundant map lookup, never a missed block.
    int64_t stride = static_cast<int64_t>(tileSize_);
    for (const auto& kv : tiles_) {
        stride = std::min(stride, static_cast<int64_t>(kv.second.tile.blockDim()));
    }

    bool ok = true;
    for (int64_t y = py0;;) {
        for (int64_t x = px0;;) {
            const FineTile* t = nullptr;
            uint32_t lx, ly;
            if (!blockFor(x, y, t, lx, ly)) ok = false;
            if (x == px1) break;
            x = (px1 - x < stride) ? px1 : x + stride;
        }
        if (y == py1) break;
        y = (py1 - y < stride) ? py1 : y + stride;
    }
    return ok;
}

} // namespace vxc
