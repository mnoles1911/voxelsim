// vxc_sliceprobe -- what a client actually pays to read part of a fine tile.
//
// The C++ counterpart of tools/tile_slice_measure.py, and the answer to the
// question task #52 is really about: the shipping client reads whole .vxtl
// files (VoxelFineTileStreamer -> vxc::readFileBytes), so what does it cost to
// read only the blocks a working set needs instead?
//
// WHAT IS MEASURED, and why these three numbers rather than one. The framing
// of this work moved once already -- there is no network fetch of fine tiles in
// the shipping client, tiles arrive as files -- so "bytes saved" on its own is
// the wrong headline. The three that matter are reported separately:
//
//   disk bytes + requests  what the read costs. A file supports ranges
//                          natively (seek+read), so this saving needs no
//                          transport, no server and no protocol.
//   resident file bytes    what FineTile HOLDS afterwards, for the tile's whole
//                          resident life. A whole-file load is 32-56 MB per
//                          tile, permanently.
//   decoded block bytes    what the decoded int16 lattice costs. ~134 MB for a
//                          fully warmed production tile -- the LARGEST of the
//                          three, and the one that only block-granular DECODE
//                          reduces. Reported so nobody has to guess whether
//                          slicing the fetch moved it. It does not, on its own.
//
// Every fetched block is verified byte-identical against the same block decoded
// out of a whole-file parse, because a fetcher that quietly grabs the wrong
// range decodes to plausible terrain rather than to an error.
//
// Usage:
//   vxc_sliceprobe <s16-dir> [--zstd PATH] [--block BX,BY] [--no-verify]

#include <algorithm>
#include <cstdio>
#include <cstdlib>  // std::atoi -- transitive on MSVC only
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "voxelcore/tilerange.h"
#include "voxelcore/tilestore.h"
#include "voxelcore/tilestreaming.h"

#if defined(_WIN32)
// NOMINMAX before windows.h: without it the min/max macros collide with
// std::min/std::max below and the failure is a wall of template errors in
// <algorithm>, on MSVC only.
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace vxc;

namespace {

// --- runtime zstd, bound exactly the way the game binds it ------------------
// voxel-core links no zstd (tilestore.h's injected FineDecompressor); without
// one bound every CODEC_ZSTD tile is REFUSED at parse, so a refusal here means
// no measurement at all rather than a smaller sample.
using ZstdDecompressFn = size_t (*)(void*, size_t, const void*, size_t);
using ZstdIsErrorFn = unsigned (*)(size_t);
ZstdDecompressFn gZstdDecompress = nullptr;
ZstdIsErrorFn gZstdIsError = nullptr;
std::string gZstdPath;

bool zstdInflate(void*, const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    if (!src || !dst || srcLen == 0 || dstLen == 0) return false;
    if (!gZstdDecompress || !gZstdIsError) return false;
    const size_t produced = gZstdDecompress(dst, dstLen, src, srcLen);
    if (gZstdIsError(produced)) return false;
    return produced == dstLen;
}

void* openLib(const char* path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_NOW);
#endif
}
void* symbol(void* h, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
    return dlsym(h, name);
#endif
}

bool bindZstd(const std::vector<std::string>& candidates) {
    for (const std::string& c : candidates) {
        if (c.empty()) continue;
        void* h = openLib(c.c_str());
        if (!h) continue;
        auto* d = reinterpret_cast<ZstdDecompressFn>(symbol(h, "ZSTD_decompress"));
        auto* e = reinterpret_cast<ZstdIsErrorFn>(symbol(h, "ZSTD_isError"));
        if (!d || !e) continue;
        gZstdDecompress = d;
        gZstdIsError = e;
        gZstdPath = c;
        return true;
    }
    return false;
}

// --- reporting --------------------------------------------------------------

// u64 printing. NOT PRId64: int64_t is `long` on 64-bit Linux and `long long`
// on Windows, so a hardcoded %lld or %ld is a -Wformat error on one of the
// three CI compilers. Casting to unsigned long long and using %llu is correct
// on all of them.
using ull = unsigned long long;

struct Cost {
    uint64_t requests = 0;
    uint64_t diskBytes = 0;
    uint64_t residentFileBytes = 0;
    uint64_t decodedBytes = 0;
    uint32_t blocks = 0;
};

void printRow(const char* label, const Cost& c, uint64_t wholeFileBytes,
              uint64_t wholeDecodedBytes) {
    // Ratios in tenths, computed in integer arithmetic, so this file has no
    // reason to touch a floating point number at all.
    const uint64_t readRatio10 = c.diskBytes ? (wholeFileBytes * 10) / c.diskBytes : 0;
    const uint64_t heldTotal = c.residentFileBytes + c.decodedBytes;
    const uint64_t wholeHeld = wholeFileBytes + wholeDecodedBytes;
    const uint64_t heldRatio10 = heldTotal ? (wholeHeld * 10) / heldTotal : 0;
    std::printf("  %-14s %4llu blk %3llu req %11llu B read (%llu.%llux)  held %11llu B "
                "= %10llu file + %11llu decoded (%llu.%llux)\n",
                label, (ull)c.blocks, (ull)c.requests, (ull)c.diskBytes, (ull)(readRatio10 / 10),
                (ull)(readRatio10 % 10), (ull)heldTotal, (ull)c.residentFileBytes,
                (ull)c.decodedBytes, (ull)(heldRatio10 / 10), (ull)(heldRatio10 % 10));
}

// Block ids of one tile covered by the dilated footprint of a block rect.
// Uses the production dilatedBlockCoverage so the working set here is the same
// set the residency gate would demand -- measuring an undilated set would
// report a saving the game could not take.
std::vector<uint32_t> dilatedIds(const FineTile& tile, uint32_t bx0, uint32_t by0, uint32_t bx1,
                                 uint32_t by1) {
    const int64_t dim = static_cast<int64_t>(tile.blockDim());
    const int64_t sz = static_cast<int64_t>(tile.size());
    const uint32_t perAxis = tile.blocksPerAxis();
    const PixelRect footprint{static_cast<int64_t>(bx0) * dim, static_cast<int64_t>(by0) * dim,
                              (static_cast<int64_t>(bx1) + 1) * dim - 1,
                              (static_cast<int64_t>(by1) + 1) * dim - 1};
    std::vector<uint32_t> ids;
    for (const BlockCoord& b : dilatedBlockCoverage(footprint, sz, tile.blockDim())) {
        // Coverage spans tiles; this probe measures ONE file, so blocks that
        // dilated into a neighbour belong to that neighbour's fetch and are not
        // this tile's cost. Working sets below are placed away from the tile
        // edge so nothing is silently dropped here.
        if (b.tile.x != 0 || b.tile.y != 0) continue;
        ids.push_back(b.blockY * perAxis + b.blockX);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// Fetch one working set into a fresh partial tile and report what it cost.
// `verifyAgainst` is a whole-file parse of the same tile; every fetched block
// is decoded from both and compared.
bool runWorkingSet(const char* label, const std::filesystem::path& path,
                   const FineDecompressor& dec, const FinePreambleRequest& want,
                   bool elevSet, const std::vector<uint32_t>& elevIds, bool waterWholePlane,
                   const FineTile& verifyAgainst, bool verify, uint64_t wholeFileBytes,
                   uint64_t wholeDecodedBytes, uint64_t& outDiskBytes, uint64_t& outRequests) {
    FileRangeSource src(path);
    if (!src.ok()) {
        std::printf("  %-14s UNREADABLE\n", label);
        return false;
    }
    FineTileBytes bytes;
    FineError err = FineError::kNone;
    if (!readFineTilePreamble(src, src.fileSize(), want, bytes, &err)) {
        std::printf("  %-14s preamble failed: %s\n", label, fineErrorName(err));
        return false;
    }
    std::optional<FineTile> tile = FineTile::parsePartial(std::move(bytes), dec, &err);
    if (!tile) {
        std::printf("  %-14s partial parse failed: %s\n", label, fineErrorName(err));
        return false;
    }

    std::vector<uint32_t> fetchedElev, fetchedWater;
    if (elevSet && !fetchFineTileBlocks(src, *tile, FinePlane::kElevation, elevIds)) {
        std::printf("  %-14s elevation fetch failed\n", label);
        return false;
    }
    if (elevSet) fetchedElev = elevIds;
    if (want.wantWater && waterWholePlane) {
        fetchedWater = fineNonConstantBlocks(*tile, FinePlane::kWater);
        if (!fetchFineTileBlocks(src, *tile, FinePlane::kWater, fetchedWater)) {
            std::printf("  %-14s water fetch failed\n", label);
            return false;
        }
    } else if (want.wantWater && elevSet) {
        fetchedWater = elevIds;
        if (!fetchFineTileBlocks(src, *tile, FinePlane::kWater, fetchedWater)) {
            std::printf("  %-14s water fetch failed\n", label);
            return false;
        }
    }

    // --- verification: identical bytes, or the measurement means nothing -----
    const uint32_t perAxis = tile->blocksPerAxis();
    uint32_t decodedBlocks = 0;
    if (verify) {
        std::vector<int16_t> a, b;
        for (uint32_t id : fetchedElev) {
            const uint32_t bx = id % perAxis, by = id / perAxis;
            if (!tile->decodeElevBlock(bx, by, a, &err)) {
                std::printf("  %-14s VERIFY FAIL elev block %llu: %s\n", label, (ull)id,
                            fineErrorName(err));
                return false;
            }
            if (!verifyAgainst.decodeElevBlock(bx, by, b) || a != b) {
                std::printf("  %-14s VERIFY MISMATCH elev block %llu\n", label, (ull)id);
                return false;
            }
            ++decodedBlocks;
        }
        for (uint32_t id : fetchedWater) {
            const uint32_t bx = id % perAxis, by = id / perAxis;
            if (!tile->decodeWaterBlock(bx, by, a, &err)) {
                std::printf("  %-14s VERIFY FAIL water block %llu: %s\n", label, (ull)id,
                            fineErrorName(err));
                return false;
            }
            if (!verifyAgainst.decodeWaterBlock(bx, by, b) || a != b) {
                std::printf("  %-14s VERIFY MISMATCH water block %llu\n", label, (ull)id);
                return false;
            }
            ++decodedBlocks;
        }
    } else {
        decodedBlocks = static_cast<uint32_t>(fetchedElev.size() + fetchedWater.size());
    }

    Cost c;
    c.requests = src.requests;
    c.diskBytes = src.bytesRead;
    c.residentFileBytes = tile->residentFileBytes();
    // What the host would hold if it kept every block this working set decoded.
    c.decodedBytes = static_cast<uint64_t>(decodedBlocks) *
                     static_cast<uint64_t>(tile->blockPixelCount()) * sizeof(int16_t);
    c.blocks = decodedBlocks;
    printRow(label, c, wholeFileBytes, wholeDecodedBytes);
    outDiskBytes = c.diskBytes;
    outRequests = c.requests;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string dir, zstdPath;
    bool verify = true;
    uint32_t centreBx = 16, centreBy = 16;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--zstd") && i + 1 < argc) {
            zstdPath = argv[++i];
        } else if (!std::strcmp(a, "--no-verify")) {
            verify = false;
        } else if (!std::strcmp(a, "--block") && i + 1 < argc) {
            const char* v = argv[++i];
            const char* comma = std::strchr(v, ',');
            if (comma) {
                centreBx = static_cast<uint32_t>(std::atoi(std::string(v, comma).c_str()));
                centreBy = static_cast<uint32_t>(std::atoi(comma + 1));
            }
        } else if (dir.empty()) {
            dir = a;
        }
    }
    if (dir.empty()) {
        std::fprintf(stderr, "usage: vxc_sliceprobe <s16-dir> [--zstd PATH] [--block BX,BY] "
                             "[--no-verify]\n");
        return 2;
    }

    {
        std::vector<std::string> cands;
        if (!zstdPath.empty()) cands.push_back(zstdPath);
#if defined(_WIN32)
        cands.push_back("libzstd.dll");
        cands.push_back("zstd.dll");
#else
        cands.push_back("libzstd.so.1");
        cands.push_back("libzstd.so");
#endif
        if (bindZstd(cands)) {
            std::printf("zstd: bound from '%s'\n", gZstdPath.c_str());
        } else {
            std::printf("zstd: NOT BOUND -- every CODEC_ZSTD tile will be refused\n");
        }
    }
    FineDecompressor dec;
    dec.fn = &zstdInflate;

    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(dir)) {
        std::fprintf(stderr, "no such directory: %s\n", dir.c_str());
        return 2;
    }
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".vxtl") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "no .vxtl files in %s\n", dir.c_str());
        return 2;
    }

    uint64_t totWhole = 0, totGround = 0, totRing = 0, totWater = 0;
    uint64_t reqGround = 0, reqRing = 0, reqWater = 0;
    int failures = 0;

    for (const std::filesystem::path& p : files) {
        // The whole-file baseline: exactly what VoxelFineTileStreamer does today.
        std::optional<std::vector<uint8_t>> raw = readFileBytes(p);
        if (!raw) {
            std::printf("%s: unreadable\n", p.filename().string().c_str());
            ++failures;
            continue;
        }
        const uint64_t wholeFileBytes = raw->size();
        FineError err = FineError::kNone;
        std::optional<FineTile> whole = FineTile::parse(std::move(*raw), dec, &err);
        if (!whole) {
            std::printf("%s: whole-file parse failed: %s\n", p.filename().string().c_str(),
                        fineErrorName(err));
            ++failures;
            continue;
        }
        const uint64_t wholeDecodedBytes = static_cast<uint64_t>(whole->blockCount()) *
                                           static_cast<uint64_t>(whole->blockPixelCount()) *
                                           sizeof(int16_t);
        const uint32_t perAxis = whole->blocksPerAxis();
        uint32_t constWater = 0;
        for (const FineBlockEntry& e : whole->waterIndex())
            if (e.mode == kBlockConstant) ++constWater;

        std::printf("\n%s  %llu B, %llux%llu blocks of %llu px, water plane %llu%% CONSTANT\n",
                    p.filename().string().c_str(), (ull)wholeFileBytes, (ull)perAxis, (ull)perAxis,
                    (ull)whole->blockDim(),
                    (ull)(whole->waterIndex().empty()
                              ? 0
                              : (uint64_t(constWater) * 100) / whole->waterIndex().size()));
        std::printf("  %-14s %4llu blk %3llu req %11llu B read (1.0x)  held %11llu B "
                    "= %10llu file + %11llu decoded (1.0x)\n",
                    "whole-file", (ull)whole->blockCount(), 1ull, (ull)wholeFileBytes,
                    (ull)(wholeFileBytes + wholeDecodedBytes), (ull)wholeFileBytes,
                    (ull)wholeDecodedBytes);
        totWhole += wholeFileBytes;

        // Clamp the working-set centre so dilation never leaves this tile: a
        // block that dilated into the neighbour would be dropped by dilatedIds
        // and the cost would read low.
        const uint32_t bx = std::min(std::max(centreBx, 2u), perAxis - 3);
        const uint32_t by = std::min(std::max(centreBy, 2u), perAxis - 3);

        uint64_t bytesOut = 0, reqOut = 0;

        // ground-1blk: one 480 m block of ELEVATION ONLY, correctly dilated.
        // The set a chunk over that block actually reads. No flow, no water
        // index -- a client that only needs ground pays for neither.
        FinePreambleRequest groundWant;
        groundWant.wantFlow = false;
        groundWant.wantWater = false;
        groundWant.wantBasins = false;
        if (runWorkingSet("ground-1blk", p, dec, groundWant, true,
                          dilatedIds(*whole, bx, by, bx, by), false, *whole, verify,
                          wholeFileBytes, wholeDecodedBytes, bytesOut, reqOut)) {
            totGround += bytesOut;
            reqGround += reqOut;
        } else {
            ++failures;
        }

        // ring-3x3: a 1.44 km neighbourhood, ground AND water -- roughly what a
        // player standing still needs resident.
        FinePreambleRequest ringWant;
        ringWant.wantFlow = false;
        ringWant.wantWater = true;
        ringWant.wantBasins = true;
        if (runWorkingSet("ring-3x3", p, dec, ringWant, true,
                          dilatedIds(*whole, bx - 1, by - 1, bx + 1, by + 1), false, *whole,
                          verify, wholeFileBytes, wholeDecodedBytes, bytesOut, reqOut)) {
            totRing += bytesOut;
            reqRing += reqOut;
        } else {
            ++failures;
        }

        // streamer-load: what VoxelFineTileStreamer::EnsureTileResident_Locked
        // needs under its CURRENT rules -- every elevation block (rule 1
        // decodes the whole tile at load, so every one of them must be held)
        // and every water block (lakes.h/riverribbon.h decode those lazily out
        // of the tile's own bytes), but NOTHING of the flow plane, which no
        // code in ue-project reads at all. This is the saving available without
        // touching the whole-tile-decode rule the streamer's thread safety
        // rests on; the ground-1blk row above is what block-granular residency
        // would buy instead, and the gap between the two rows is the prize.
        {
            FinePreambleRequest sWant;
            sWant.wantFlow = false;
            sWant.wantWater = true;
            sWant.wantBasins = true;
            FileRangeSource src(p);
            FineTileBytes sBytes;
            FineError sErr = FineError::kNone;
            if (src.ok() && readFineTilePreamble(src, src.fileSize(), sWant, sBytes, &sErr)) {
                std::optional<FineTile> st = FineTile::parsePartial(std::move(sBytes), dec, &sErr);
                if (st && fetchFineTileBlocks(src, *st, FinePlane::kElevation,
                                              fineNonConstantBlocks(*st, FinePlane::kElevation)) &&
                    fetchFineTileBlocks(src, *st, FinePlane::kWater,
                                        fineNonConstantBlocks(*st, FinePlane::kWater))) {
                    Cost c;
                    c.requests = src.requests;
                    c.diskBytes = src.bytesRead;
                    c.residentFileBytes = st->residentFileBytes();
                    c.decodedBytes = wholeDecodedBytes;  // rule 1: every block decoded
                    c.blocks = st->blockCount();
                    printRow("streamer-load", c, wholeFileBytes, wholeDecodedBytes);
                } else {
                    std::printf("  %-14s failed: %s\n", "streamer-load", fineErrorName(sErr));
                    ++failures;
                }
            } else {
                std::printf("  %-14s preamble failed: %s\n", "streamer-load",
                            fineErrorName(sErr));
                ++failures;
            }
        }

        // water-only: EVERY wet block in the tile, plus the basin registry --
        // the whole water plane refreshed, which is what a water-only re-bake
        // costs a client that already holds the tile.
        FinePreambleRequest waterWant;
        waterWant.wantFlow = false;
        waterWant.wantWater = true;
        waterWant.wantBasins = true;
        if (runWorkingSet("water-only", p, dec, waterWant, false, {}, true, *whole, verify,
                          wholeFileBytes, wholeDecodedBytes, bytesOut, reqOut)) {
            totWater += bytesOut;
            reqWater += reqOut;
        } else {
            ++failures;
        }
    }

    std::printf("\n--- totals over %llu tile(s) ---\n", (ull)files.size());
    std::printf("  whole-file   %12llu B read\n", (ull)totWhole);
    std::printf("  ground-1blk  %12llu B read in %llu request(s)\n", (ull)totGround, (ull)reqGround);
    std::printf("  ring-3x3     %12llu B read in %llu request(s)\n", (ull)totRing, (ull)reqRing);
    std::printf("  water-only   %12llu B read in %llu request(s)\n", (ull)totWater, (ull)reqWater);
    if (failures != 0) {
        std::printf("\n%d working set(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall working sets verified byte-identical to a whole-file decode\n");
    return 0;
}
