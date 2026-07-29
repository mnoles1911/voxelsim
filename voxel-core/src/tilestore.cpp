#include "voxelcore/tilestore.h"

#include <algorithm>
#include <fstream>

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
        return 0;
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
bool parseBlockIndex(const uint8_t* file, uint64_t indexOff, uint64_t indexLen,
                     uint32_t blockCount, uint32_t blockPixels, uint64_t dataLen,
                     uint32_t bytesPerSample, bool compressed,
                     std::vector<FineBlockEntry>& out) {
    if (indexLen != static_cast<uint64_t>(blockCount) * kFineBlockEntryBytes) return false;
    ByteReader r(file + indexOff, static_cast<size_t>(indexLen));

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

} // namespace

const char* fineErrorName(FineError e) {
    switch (e) {
    case FineError::kNone: return "none";
    case FineError::kFileUnreadable: return "file-unreadable";
    case FineError::kNotVxtl: return "not-a-vxtl";
    case FineError::kWrongVersion: return "wrong-version";
    case FineError::kBadHeader: return "bad-header";
    case FineError::kUnknownCodec: return "unknown-codec";
    case FineError::kNoDecompressor: return "no-decompressor";
    case FineError::kBadSectionTable: return "bad-section-table";
    case FineError::kBadBlockIndex: return "bad-block-index";
    case FineError::kBadBlockCoords: return "bad-block-coords";
    case FineError::kDecompressFailed: return "decompress-failed";
    case FineError::kBadPayload: return "bad-payload";
    case FineError::kValueOutOfRange: return "value-out-of-range";
    }
    return "unknown";
}

std::optional<FineTile> FineTile::parse(const uint8_t* data, size_t size,
                                        const FineDecompressor& decompressor, FineError* err) {
    return FineTile::parse(std::vector<uint8_t>(data, data + size), decompressor, err);
}

std::optional<FineTile> FineTile::parse(std::vector<uint8_t> bytes,
                                        const FineDecompressor& decompressor, FineError* err) {
    if (err) *err = FineError::kNone;
    // Every failing return below funnels through this, so "returned nullopt"
    // and "recorded a reason" can never drift apart.
    const auto reject = [err](FineError e) -> std::optional<FineTile> {
        if (err) *err = e;
        return std::nullopt;
    };

    const size_t fileSize = bytes.size();
    ByteReader r(bytes.data(), fileSize);

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
    if ((h.flags & ~kFineFlagFlowPresent) != 0) return reject(FineError::kBadHeader);
    if (!r.i32(h.baseOffsetMm)) return reject(FineError::kBadHeader);
    // §3: absolute only.
    if (!r.u8(h.parentScale) || h.parentScale != 0) return reject(FineError::kBadHeader);
    for (int i = 0; i < 3; ++i) {
        uint8_t reserved;
        if (!r.u8(reserved) || reserved != 0) return reject(FineError::kBadHeader);
    }

    uint16_t nSections;
    if (!r.u16(nSections)) return reject(FineError::kBadHeader);

    const uint64_t tableEnd =
        static_cast<uint64_t>(kFineHeaderBytes) +
        static_cast<uint64_t>(nSections) * kFineSectionEntryBytes;
    if (tableEnd > fileSize) return reject(FineError::kBadSectionTable);

    SectionRef elevIndexSec, elevDataSec, flowIndexSec, flowDataSec;
    struct Extent {
        uint64_t begin, end;
    };
    std::vector<Extent> extents;
    std::vector<uint32_t> seenIds;
    uint64_t coveredEnd = tableEnd;

    for (uint16_t i = 0; i < nSections; ++i) {
        uint32_t id;
        uint64_t off, len;
        if (!r.u32(id) || !r.u64(off) || !r.u64(len)) return reject(FineError::kBadSectionTable);

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

    FineTile tile;
    tile.h_ = h;
    tile.dec_ = decompressor;
    tile.elevDataOff_ = elevDataSec.offset;
    tile.elevDataLen_ = elevDataSec.length;
    tile.flowDataOff_ = flowDataSec.offset;
    tile.flowDataLen_ = flowDataSec.length;

    const uint32_t perAxis = static_cast<uint32_t>(h.size) >> h.blockLog2;
    const uint32_t blocks = perAxis * perAxis;
    const uint32_t dim = 1u << h.blockLog2;
    const uint32_t blockPixels = dim * dim;

    if (!parseBlockIndex(bytes.data(), elevIndexSec.offset, elevIndexSec.length, blocks,
                         blockPixels, elevDataSec.length, 2, compressed, tile.elevIndex_)) {
        return reject(FineError::kBadBlockIndex);
    }
    if (wantFlow && !parseBlockIndex(bytes.data(), flowIndexSec.offset, flowIndexSec.length,
                                     blocks, blockPixels, flowDataSec.length, 1, compressed,
                                     tile.flowIndex_)) {
        return reject(FineError::kBadBlockIndex);
    }

    tile.bytes_ = std::move(bytes);
    return tile;
}

bool FineTile::decodeElevBlock(uint32_t bx, uint32_t by, std::vector<int16_t>& out,
                               FineError* err) const {
    if (err) *err = FineError::kNone;
    const uint32_t perAxis = blocksPerAxis();
    if (bx >= perAxis || by >= perAxis) return fail(err, FineError::kBadBlockCoords);
    const uint32_t dim = blockDim();
    out.assign(blockPixelCount(), 0);
    return decodeBlockPayload<int16_t>(elevIndex_[by * perAxis + bx],
                                       bytes_.data() + elevDataOff_,
                                       static_cast<size_t>(elevDataLen_), dim, -32768, 32767,
                                       dec_, fineCodecNeedsDecompressor(h_.codec), out.data(),
                                       err);
}

bool FineTile::decodeFlowBlock(uint32_t bx, uint32_t by, std::vector<uint8_t>& out,
                               FineError* err) const {
    if (err) *err = FineError::kNone;
    if (!hasFlow()) return fail(err, FineError::kBadBlockCoords);
    const uint32_t perAxis = blocksPerAxis();
    if (bx >= perAxis || by >= perAxis) return fail(err, FineError::kBadBlockCoords);
    const uint32_t dim = blockDim();
    out.assign(blockPixelCount(), 0);
    return decodeBlockPayload<uint8_t>(flowIndex_[by * perAxis + bx],
                                       bytes_.data() + flowDataOff_,
                                       static_cast<size_t>(flowDataLen_), dim, 0, 255, dec_,
                                       fineCodecNeedsDecompressor(h_.codec), out.data(), err);
}

bool FineTile::controlPointAt(uint32_t lx, uint32_t ly, int16_t& cp) const {
    if (lx >= h_.size || ly >= h_.size) return false;
    const uint32_t dim = blockDim();
    std::vector<int16_t> block;
    if (!decodeElevBlock(lx >> h_.blockLog2, ly >> h_.blockLog2, block)) return false;
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
        if (!res.tile.decodeElevBlock(bx, by, decoded)) {
            blockDecodeFailures.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        bit = res.blocks.emplace(key, std::move(decoded)).first;
    }
    tile = &res.tile;
    return &bit->second;
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
    if (!block) return 0;
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
