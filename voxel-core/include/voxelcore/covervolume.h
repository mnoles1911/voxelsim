#pragma once
// THE CPU COVER PRODUCER -- detail-lattice ground cover, packed into the
// canonical brick format on its own 5 cm lattice.
//
// docs/detail-assets-in-the-volume-2026-08-19.md is the design and the
// measurement; docs/measurements/cover-volume-census-2026-08-19.txt is the
// number this producer exists to make real (26.5 MiB at grassland over the
// 112 m ring, against a 500 MiB falsifier and a 31.3 MiB terrain control on the
// same ground).
//
// WHAT THIS IS, IN ONE SENTENCE. `packChunkBricksCanonical` over
// `AssetField::coverMaterialOfResolved` -- the shipping packer, handed a cover
// accessor instead of a terrain one. Its own header says the reuse is intended:
// "packed by handing it a coarse accessor and nothing else changes."
//
// WHY THAT MATTERS MORE THAN THE CODE SAVING. Because the producer IS the
// packer, a GPU cover stamp is checkable against this from its first day by the
// gate that already exists (voxel.GPU.VerifyBrickPack's shape), not by a new
// one written afterwards. That is docs/...-2026-08-19.md section 7.2 applied to
// its own phase: the reference is built before the gate. The composition half
// of the reference -- resolveForCoverCompose / coverMaterialOfResolved -- is in
// assetfield.h and is mutation-proved; this file is the packing half.
//
// ---------------------------------------------------------------------------
// DESIGN REQUIREMENT C1: SPARSE, AND IT IS NOT A PREFERENCE
// ---------------------------------------------------------------------------
// Measured: a flat 3D brick index over the cover footprint costs 290.1 MiB at
// the alpine site against 25.4 MiB at grassland -- an 11x swing driven by
// TERRAIN RELIEF, not by cover, because a thin shell following the ground is
// 0.075% occupied inside its own bounding box on steep slopes. The payload
// never came near the falsifier; a dense index is the only term that did.
//
// So this producer answers PER CHUNK and answers `anyCover = false` for a chunk
// with nothing in it. A caller must store nothing for those chunks -- not a
// zeroed pack, not a reserved slot. Anyone who wants a dense array here has to
// price it at the steepest ground it will ever run on, not the flattest.
//
// ---------------------------------------------------------------------------
// THE COUNTERS EXIST TO FAIL
// ---------------------------------------------------------------------------
// This project found ten instruments in one night that reported plausibly while
// measuring nothing, two of them counters that could not fail. So the three
// chunk counters below are a strict funnel and they are separate on purpose:
//
//   chunksAttempted  the producer was entered at all
//   chunksResolved   ... and the resolve returned at least one usable instance
//   chunksPacked     ... and the pack held at least one solid voxel
//
// attempted == 0                 -> THE PRODUCER DID NOT RUN (cvar off, gate
//                                   closed, ring empty). Not a cover result.
// attempted > 0, packed == 0     -> IT RAN AND PRODUCED NOTHING. That is a
//                                   real, reportable answer about the ground.
// A single "coverChunks" counter cannot tell those apart, and the difference is
// exactly what the alpine site turned out to be (measured: only three species
// place there; no grass, no flowers, no reeds).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "voxelcore/assetfield.h"
#include "voxelcore/assetmanifest.h"
#include "voxelcore/brickpack.h"
#include "voxelcore/core.h"

namespace vxc {

// The cover chunk is the march chunk's shape on the cover lattice: 32 cover
// cells per axis, so 1.6 m at 50 mm. Named here rather than assumed so the two
// cannot drift silently.
inline constexpr int32_t kCoverChunkEdgeCells = kMarchChunkEdgeVoxels;

struct CoverProducerCounters {
    std::atomic<uint64_t> chunksAttempted{0};
    std::atomic<uint64_t> chunksResolved{0};
    std::atomic<uint64_t> chunksPacked{0};
    // Anchor-owned only. Reach dilation returns one instance to every chunk it
    // overlaps, so counting list sizes over-counts; measured against
    // docs/biome-placement-survey.md, only the anchor rule agrees (3,717/ha
    // against an independently walked 3,908/ha).
    std::atomic<uint64_t> instancesAnchored{0};
    std::atomic<uint64_t> solidVoxels{0};
    std::atomic<uint64_t> residentBytes{0};

    void reset() {
        chunksAttempted.store(0, std::memory_order_relaxed);
        chunksResolved.store(0, std::memory_order_relaxed);
        chunksPacked.store(0, std::memory_order_relaxed);
        instancesAnchored.store(0, std::memory_order_relaxed);
        solidVoxels.store(0, std::memory_order_relaxed);
        residentBytes.store(0, std::memory_order_relaxed);
    }
    uint64_t attempted() const { return chunksAttempted.load(std::memory_order_relaxed); }
    uint64_t resolved() const { return chunksResolved.load(std::memory_order_relaxed); }
    uint64_t packed() const { return chunksPacked.load(std::memory_order_relaxed); }
    uint64_t anchored() const { return instancesAnchored.load(std::memory_order_relaxed); }
    uint64_t solid() const { return solidVoxels.load(std::memory_order_relaxed); }
    uint64_t bytes() const { return residentBytes.load(std::memory_order_relaxed); }

    // "Ran and produced nothing" -- a real answer, distinguishable from silence.
    bool ranAndFoundNothing() const { return attempted() > 0 && packed() == 0; }
};

// --- init: refuse the pitch, and name the content it cannot hold -------------
//
// A cover volume admits exactly one bake (nothing in voxel-core resamples), so
// every detail species baked at another pitch is dropped -- correctly, and
// silently unless something says so at load. MEASURED, and naming them changed
// what they are: at 50 mm the refusals are black-coral-tree,
// branching-stony-coral, carnation-soft-coral, cold-water-coral, elkhorn-coral,
// leather-coral and staghorn-coral -- all seven are REEF, weighted ocean-only
// and zero in every land biome. A land cover volume refuses nothing it would
// ever have drawn. An unnamed "7 dropped" would have been debugged as a defect
// in the land path, which is the whole argument for refusing by name.
struct CoverVolumeInit {
    bool ok = false;
    uint32_t pitchMm = 0;
    std::vector<AssetCoverPitchRefusal> refused;
    const char* error = nullptr;
};

inline CoverVolumeInit coverVolumeInit(const AssetManifest& manifest, uint32_t pitchMm) {
    CoverVolumeInit out;
    out.pitchMm = pitchMm;
    if (pitchMm == 0) {
        out.error = "cover pitch of zero";
        return out;
    }
    // The cover lattice must TILE the world lattice or the standing-on-top z
    // convention is not expressible (see resolveForCoverCompose).
    if (uint32_t(kVoxelSizeMm) % pitchMm != 0) {
        out.error = "cover pitch does not divide the world voxel size";
        return out;
    }
    out.refused = assetCoverPitchRefusals(manifest, pitchMm);
    out.ok = true;
    return out;
}

// --- one chunk ---------------------------------------------------------------

struct CoverChunkResult {
    ChunkBrickPack pack;
    // FALSE MEANS STORE NOTHING. Requirement C1: no zeroed pack, no reserved
    // slot, no dense array entry.
    bool anyCover = false;
    int32_t instancesAnchored = 0;
    // The list the pack was built from, returned rather than discarded so a
    // gate can compare the PACK against coverMaterialAtResolved over the SAME
    // instances. A test that re-derives the list from its own rect is checking
    // two things at once and cannot say which failed.
    std::vector<AssetField::ResolvedCoverInstance> resolved;
};

// Produce one cover chunk at COVER-chunk coordinates (cx, cy, cz).
//
// `columnFacts` is the caller's binding into the world -- the same callable
// AssetField::instancesForRect takes everywhere else, so this file needs no
// amplifier, no tiles and no engine. THE INSTRUMENT MUST RUN THE ENGINE'S
// BINDING: a caller that passes a channel-less facts function is censusing the
// sentinel world in which riparians refuse everywhere, which is a different
// world and not this one.
template <typename ColumnFactsFn>
CoverChunkResult produceCoverChunk(const AssetField& field, uint32_t pitchMm, int64_t cx,
                                   int64_t cy, int64_t cz, const ColumnFactsFn& columnFacts,
                                   CoverProducerCounters& counters) {
    CoverChunkResult out;
    counters.chunksAttempted.fetch_add(1, std::memory_order_relaxed);
    if (pitchMm == 0 || uint32_t(kVoxelSizeMm) % pitchMm != 0) return out;

    const int64_t E = int64_t(kCoverChunkEdgeCells);
    const int64_t baseCx = cx * E, baseCy = cy * E, baseCz = cz * E;

    // The chunk's own rect in LEVEL-0 voxels. assetSitesForRect already dilates
    // by each layer's maxRadiusMm and tests every site's reach exactly, so
    // pre-dilating here would enumerate twice (assetfield.h's own note).
    const int64_t p = int64_t(pitchMm);
    const AssetVoxelRect rect{floorDiv(baseCx * p, int64_t(kVoxelSizeMm)),
                              floorDiv(baseCy * p, int64_t(kVoxelSizeMm)),
                              floorDiv((baseCx + E) * p - 1, int64_t(kVoxelSizeMm)),
                              floorDiv((baseCy + E) * p - 1, int64_t(kVoxelSizeMm))};

    // terrainOnly FALSE -- the whole point. Every other UE caller passes true,
    // which is what makes these 85% of instances invisible to the world volume.
    const std::vector<AssetInstance> insts =
        field.instancesForRect(rect, columnFacts, /*terrainOnly*/ false);
    if (insts.empty()) return out;
    out.resolved = field.resolveForCoverCompose(insts, pitchMm);
    const std::vector<AssetField::ResolvedCoverInstance>& resolved = out.resolved;
    if (resolved.empty()) return out;
    counters.chunksResolved.fetch_add(1, std::memory_order_relaxed);

    // Anchor ownership is the dedup; see the counter's comment.
    for (const auto& r : resolved)
        if (r.anchorCx >= baseCx && r.anchorCx < baseCx + E && r.anchorCy >= baseCy &&
            r.anchorCy < baseCy + E)
            ++out.instancesAnchored;
    counters.instancesAnchored.fetch_add(uint64_t(out.instancesAnchored),
                                         std::memory_order_relaxed);

    // Z REJECT BEFORE ANYTHING EXPENSIVE. A cover volume is mostly empty
    // vertically -- cover is a metre of plants over a landscape's whole relief
    // -- so the common case is a chunk the instances do not reach at all. This
    // is the sparse economy of C1 in the producer rather than in the store.
    bool anyZ = false;
    for (const auto& r : resolved) {
        const int64_t lo = r.anchorCz + int64_t(r.grid->originZ());
        const int64_t hi = lo + int64_t(r.grid->sizeZ()) - 1;
        if (hi >= baseCz && lo < baseCz + E) {
            anyZ = true;
            break;
        }
    }
    if (!anyZ) return out;

    // Per-column shortlists over the chunk's own 32x32, instance-outward. Cover
    // boxes are already in COVER cells, so this is a clamp and not a lattice
    // conversion.
    std::array<std::vector<uint16_t>, size_t(kCoverChunkEdgeCells) * size_t(kCoverChunkEdgeCells)>
        shortlist;
    for (size_t i = 0; i < resolved.size(); ++i) {
        const auto& r = resolved[i];
        const int64_t x0 = r.anchorCx + r.grid->rotatedOriginX(r.yawQuarter);
        const int64_t y0 = r.anchorCy + r.grid->rotatedOriginY(r.yawQuarter);
        const int64_t x1 = x0 + r.grid->rotatedSizeX(r.yawQuarter) - 1;
        const int64_t y1 = y0 + r.grid->rotatedSizeY(r.yawQuarter) - 1;
        const int64_t lx0 = std::max<int64_t>(x0 - baseCx, 0);
        const int64_t ly0 = std::max<int64_t>(y0 - baseCy, 0);
        const int64_t lx1 = std::min<int64_t>(x1 - baseCx, E - 1);
        const int64_t ly1 = std::min<int64_t>(y1 - baseCy, E - 1);
        for (int64_t ly = ly0; ly <= ly1; ++ly)
            for (int64_t lx = lx0; lx <= lx1; ++lx)
                shortlist[size_t(lx) + size_t(kCoverChunkEdgeCells) * size_t(ly)].push_back(
                    static_cast<uint16_t>(i));
    }

    // THE PRODUCER IS THE PACKER. Chunk-local (x, y, z) in [0, 32); every cell
    // comes through coverMaterialOfResolved, the one place the cover transform
    // is spelled, so this cannot drift from the reference a GPU mirror is
    // checked against.
    out.pack = packChunkBricksCanonical([&](int32_t x, int32_t y, int32_t z) -> MaterialId {
        const std::vector<uint16_t>& sl =
            shortlist[size_t(x) + size_t(kCoverChunkEdgeCells) * size_t(y)];
        for (const uint16_t i : sl) {
            const MaterialId m = AssetField::coverMaterialOfResolved(
                resolved[i], baseCx + x, baseCy + y, baseCz + z);
            if (m != MAT_AIR) return m;
        }
        return MAT_AIR;   // no terrain term: a cover volume holds cover
    });

    out.anyCover = out.pack.anySolid;
    if (!out.anyCover) return out;   // ran, produced nothing -- store nothing

    counters.chunksPacked.fetch_add(1, std::memory_order_relaxed);
    counters.residentBytes.fetch_add(uint64_t(out.pack.residentBytes()),
                                     std::memory_order_relaxed);
    uint64_t solid = 0;
    for (int32_t x = 0; x < kCoverChunkEdgeCells; ++x)
        for (int32_t y = 0; y < kCoverChunkEdgeCells; ++y)
            for (int32_t z = 0; z < kCoverChunkEdgeCells; ++z)
                if (decodeChunkVoxelCanonical(out.pack, x, y, z) != MAT_AIR) ++solid;
    counters.solidVoxels.fetch_add(solid, std::memory_order_relaxed);
    return out;
}

} // namespace vxc
