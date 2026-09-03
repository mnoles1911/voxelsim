#pragma once
// THE CPU CRAFT PRODUCER -- one promoted terrain brick, packed into the
// canonical brick format on its own 25 mm lattice.
//
// WHAT THIS IS, IN ONE SENTENCE. packChunkBricksCanonical over
// CraftLattice::materialAt -- the shipping packer, handed a craft accessor
// instead of a terrain one. This is covervolume.h's construction exactly, and
// covervolume.h's own header says the reuse is intended: "packed by handing it
// a coarse accessor and nothing else changes."
//
// WHY THAT MATTERS MORE THAN THE CODE SAVING. Because the producer IS the
// packer, a GPU craft stamp is checkable against this from its first day by the
// gate that already exists (voxel.Cover.VerifyStore's shape), not by a new one
// written afterwards.
//
// THE PACKER NEEDS NO CHANGE AT ALL. kCraftChunkEdgeCells == kMarchChunkEdgeVoxels
// (32) and kVoxelSizeMm % kCraftPitchMm == 0, so a craft chunk is a canonical
// 32^3 brick chunk in every respect -- 64 descriptors, 16 occupancy dwords per
// mixed brick, a fixed 16 B local palette, popcount-compacted payload.
//
// ---------------------------------------------------------------------------
// WHERE THIS DIVERGES FROM COVER, AND IT IS NOT A DETAIL
// ---------------------------------------------------------------------------
// Cover obeys requirement C1: a chunk with nothing in it stores NOTHING -- no
// zeroed pack, no reserved slot -- because a cover volume is mostly empty and a
// dense store over its bounding box was measured at 290.1 MiB against 25.4 MiB
// of payload.
//
// A CRAFT CHUNK WITH NO SOLID CELLS MUST STILL BE STORED. The marcher's terrain
// walk skips a brick whose supersede bit is set, and that bit means "a craft
// chunk for this brick is resident". A player who hollows a brick out
// completely produces an all-air craft chunk -- and if the producer drops it
// the way cover would, the supersede bit clears, the terrain walk stops
// skipping, and THE CARVED-AWAY ROCK COMES BACK. The empty pack is the whole
// point of the empty pack.
//
// So the sparsity here is in WHICH BRICKS ARE PROMOTED, not in whether a
// promoted brick produces. C1 is satisfied by promotion being deliberate.
//
// ---------------------------------------------------------------------------
// THE COUNTERS EXIST TO FAIL
// ---------------------------------------------------------------------------
//   chunksAttempted == 0            -> THE PRODUCER DID NOT RUN (cvar off, no
//                                      promoted bricks in band). Not a result.
//   attempted > 0, produced == 0    -> it ran and refused everything. A defect.
//   produced > 0, withSolid == 0    -> every promoted brick is hollow. A real,
//                                      reportable answer about the world, and
//                                      NOT the same reading as the line above.
//   refusedMissingBrick > 0         -> a promoted brick lost craft bricks. This
//                                      is the one that would otherwise render
//                                      as deleted terrain with everything green.

#include <atomic>
#include <cstdint>

#include "voxelcore/brick.h"
#include "voxelcore/brickpack.h"
#include "voxelcore/core.h"
#include "voxelcore/craftlattice.h"

namespace vxc {

struct CraftProducerCounters {
    std::atomic<uint64_t> chunksAttempted{0};
    std::atomic<uint64_t> chunksProduced{0};
    std::atomic<uint64_t> chunksWithSolid{0};
    std::atomic<uint64_t> refusedNotPromoted{0};
    std::atomic<uint64_t> refusedMissingBrick{0};
    std::atomic<uint64_t> solidCellsPacked{0};

    void reset() {
        chunksAttempted = 0;
        chunksProduced = 0;
        chunksWithSolid = 0;
        refusedNotPromoted = 0;
        refusedMissingBrick = 0;
        solidCellsPacked = 0;
    }

    // The one line a stats command prints. Says the words "did not run" rather
    // than a row of zeroes, because zeroes are what a broken instrument and an
    // unchiselled world have in common.
    bool ran() const { return chunksAttempted.load(std::memory_order_relaxed) != 0; }
};

struct CraftChunkResult {
    ChunkBrickPack pack;

    // The pack is valid and must be stored. NOT the same as "has solid cells":
    // a hollowed-out promoted brick produces an all-air pack that MUST be
    // resident, or the supersede bit clears and the terrain reappears.
    bool produced = false;

    // Whether anything in it is non-air. Reporting only -- never a store gate.
    bool anySolid = false;

    int64_t residentBytes() const { return produced ? pack.residentBytes() : 0; }
};

// Pack ONE promoted terrain brick into a craft chunk.
//
// The craft chunk key IS the terrain brick key (craftlattice.h): a craft chunk
// is 32 cells x 25 mm = 800 mm = 8 * kVoxelSizeMm. So this takes a terrain
// BrickKey and needs no second coordinate space.
template <int B>
CraftChunkResult produceCraftChunk(const CraftLattice<B>& lattice, const BrickKey& terrainBrick,
                                   CraftProducerCounters& counters) {
    CraftChunkResult out;
    counters.chunksAttempted.fetch_add(1, std::memory_order_relaxed);

    if (!lattice.isPromoted(terrainBrick)) {
        counters.refusedNotPromoted.fetch_add(1, std::memory_order_relaxed);
        return out;
    }

    // Resolve the 64 craft bricks ONCE, in chunkBrickIndex order, so the packer
    // accessor is pointer arithmetic rather than 32,768 hash lookups.
    //
    // A MISSING BRICK IS A REFUSAL, NEVER AIR. Handing the packer a null and
    // letting the cell read MAT_AIR is the "absence reads as air" shape this
    // project has paid for repeatedly, and here it would delete exactly the
    // terrain the supersede bit has already told the marcher to skip.
    using CraftBrick = typename CraftLattice<B>::CraftBrick;
    const CraftBrick* bricks[kMarchChunkBricks] = {};
    const BrickKey base = craftBrickBaseOfTerrainBrick(terrainBrick);
    for (int32_t bz = 0; bz < kCraftBricksPerAxis; ++bz)
        for (int32_t by = 0; by < kCraftBricksPerAxis; ++by)
            for (int32_t bx = 0; bx < kCraftBricksPerAxis; ++bx) {
                const CraftBrick* c =
                    lattice.craftBricks().find(BrickKey{base.x + bx, base.y + by, base.z + bz});
                if (c == nullptr) {
                    counters.refusedMissingBrick.fetch_add(1, std::memory_order_relaxed);
                    return out;
                }
                bricks[chunkBrickIndex(bx, by, bz)] = c;
            }

    // THE PRODUCER IS THE PACKER. Chunk-local (x, y, z) in [0, 32).
    out.pack = packChunkBricksCanonical([&](int32_t x, int32_t y, int32_t z) -> MaterialId {
        const int32_t bi = chunkBrickIndex(x / kMarchBrickEdge, y / kMarchBrickEdge,
                                           z / kMarchBrickEdge);
        return bricks[bi]->get(x % kMarchBrickEdge, y % kMarchBrickEdge, z % kMarchBrickEdge);
    });

    out.produced = true;
    out.anySolid = out.pack.anySolid;
    counters.chunksProduced.fetch_add(1, std::memory_order_relaxed);
    if (out.anySolid) {
        counters.chunksWithSolid.fetch_add(1, std::memory_order_relaxed);
        uint64_t solid = 0;
        for (int32_t bi = 0; bi < kMarchChunkBricks; ++bi)
            solid += static_cast<uint64_t>(bricks[bi]->solidCount());
        counters.solidCellsPacked.fetch_add(solid, std::memory_order_relaxed);
    }
    return out;
}

} // namespace vxc
