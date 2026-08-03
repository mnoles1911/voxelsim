#pragma once
// Cave FAMILY ATTRIBUTION -- instrumentation only (docs/underground-system-plan.md W1).
//
// ---------------------------------------------------------------------------
// THIS IS NOT PART OF THE WORLDGEN CONTRACT
// ---------------------------------------------------------------------------
// Nothing here is called by Amplifier, by GeneratedWorld, or by anything that
// generates a voxel. Nothing here is mirrored in voxel-core/shaders/worldgen.ush
// and nothing here needs to be: it computes no new geometry and decides no
// material. It only asks the SHIPPING predicates a question they cannot
// otherwise answer -- "which generator carved this voxel?" -- and it is used by
// vxc_caveprobe and by test_caves.cpp. It carries no kWorldGenVersion
// obligation, and a change here can never move a digest.
//
// It lives in include/ rather than being copied into both callers because two
// hand-kept copies of an attribution rule is exactly the drift this codebase
// refuses elsewhere. It is integer-only like everything else here.
//
// ---------------------------------------------------------------------------
// WHY IT HAS TO EXIST AT ALL
// ---------------------------------------------------------------------------
// caveColumnFor reduces tunnels AND crevices into one flat `segs[]` array with
// no provenance tag (caves.h's CaveSeg is deliberately two int32s and nothing
// else -- that is what makes the per-voxel test one multiply and one compare).
// So a carved voxel carries no family label, and "how much of the underground
// is crevice?" is unanswerable from the shipped types.
//
// The obvious fix -- re-deriving the closest-approach geometry here and
// labelling as we go -- would be a second implementation of worldgen, which is
// the drift-prone thing the plan says must never be done. So instead the
// attribution DIFFERENCES the shipping predicate over controlled variants of
// the lattice block:
//
//   L      the real CaveLattice for the column's cell   -> tunnels+crevices+shaft
//   Lns    L with shaftNodeSlot = -1                    -> tunnels+crevices
//   Lt     Lns with every crevHash forced gate-CLOSED   -> tunnels only
//
// caveColumnFromLattice is a pure function of the block and the tunnel
// emission never reads crevHash, so the tunnel-only column's segs are
// bit-identical to the tunnel subset of the real column's. No geometry is
// recomputed anywhere below; the only arithmetic is caves.h's own.
//
// The result is CHECKABLE, and callers are expected to check it:
// caveCarveAt(full) must equal (shaft || tunnel || crevice) at every voxel.
// `caveFamilyMaskAt` returns that ground truth through `caveTruthOut` for
// exactly that purpose, so a wrong instrument reports itself instead of
// reporting a plausible number.

#include "voxelcore/caverns.h"
#include "voxelcore/caves.h"
#include "voxelcore/core.h"

namespace vxc {

// Bit positions, because a voxel can legitimately belong to more than one
// family: a shaft's bottom IS a backbone crossing node, so its lowest voxels
// sit inside the four tunnels meeting there.
enum : uint32_t {
    CAVE_FAM_TUNNEL = 0,
    CAVE_FAM_CREVICE = 1,
    CAVE_FAM_SHAFT = 2,
    CAVE_FAM_CAVERN = 3,
    kCaveFamilyCount = 4,
};

// Shaft first, deliberately. It is the entrance, and an entrance relabelled
// "tunnel" because its bottom sits on a junction would make the plan's
// headline perforation statistic meaningless.
inline constexpr uint32_t kCaveFamilyPriority[kCaveFamilyCount] = {
    CAVE_FAM_SHAFT, CAVE_FAM_CAVERN, CAVE_FAM_TUNNEL, CAVE_FAM_CREVICE};

// A crevHash whose gate is CLOSED. caveCreviceGateOpen tests
// ((h >> 61) & kCrevGateMask) == 0, so any value with a set bit in 61..63
// closes it for every seed.
inline constexpr uint64_t kCaveCrevHashClosed = 1ull << 61;

struct CaveColumnVariants {
    CaveColumn full;    // tunnels + crevices + shaft (== ColumnSample::cave)
    CaveColumn tunnel;  // tunnels only
    CaveColumn tunCrev; // tunnels + crevices, no shaft
};

// `full` is bit-identical to what caveColumnFor(seed, vx, vy, surfaceMm)
// returns, including its below-threshold empty case -- callers cross-check
// that against the ColumnSample they already have.
template <typename SurfaceFn>
inline CaveColumnVariants caveColumnVariantsFor(uint64_t seed, int64_t vx, int64_t vy,
                                                int32_t surfaceMm, const SurfaceFn& surfaceAt) {
    CaveColumnVariants out;
    if (surfaceMm < kCaveMinSurfaceMm) return out; // caveColumnFor's own guard
    const int64_t ci = floorDiv(vx * kVoxelSizeMm, kCaveLatticeMm);
    const int64_t cj = floorDiv(vy * kVoxelSizeMm, kCaveLatticeMm);
    const CaveLattice L = caveLatticeFor(seed, ci, cj, surfaceAt);

    CaveLattice lns = L;
    lns.shaftNodeSlot = -1;
    CaveLattice lt = lns;
    for (CaveLatticeEdge& e : lt.edges) e.crevHash = kCaveCrevHashClosed;

    out.full = caveColumnFromLattice(seed, L, vx, vy, surfaceMm);
    out.tunCrev = caveColumnFromLattice(seed, lns, vx, vy, surfaceMm);
    out.tunnel = caveColumnFromLattice(seed, lt, vx, vy, surfaceMm);
    return out;
}

// The shaft branch of caveCarveAt on its own. Deliberately kept in the same
// shape and order as caves.h:547-564 so it can be diffed by eye; the
// ground-truth cross-check is what proves it has not drifted.
constexpr bool caveInShaftBranch(const CaveColumn& c, int32_t surfaceMm, int64_t vz) {
    if (c.shaftMarginSq <= 0) return false;
    if (vz < kCaveMinVoxelZ) return false;
    if (surfaceMm < kCaveMinSurfaceMm) return false;
    const int64_t depthMm =
        static_cast<int64_t>(surfaceMm) - (vz * kVoxelSizeMm + kVoxelSizeMm / 2);
    if (depthMm < 0) return false;
    return depthMm >= static_cast<int64_t>(c.shaftDepthMinMm) &&
           depthMm <= static_cast<int64_t>(c.shaftDepthMaxMm);
}

// Family bitmask for one voxel. `caveTruthOut` receives caveCarveAt's own
// answer for the same voxel: callers MUST compare it against the tunnel /
// crevice / shaft bits, because that comparison is the only thing standing
// between this file and a confidently wrong census.
inline uint32_t caveFamilyMaskAt(const CaveColumnVariants& cv, const CavernColumn& cavern,
                                 int32_t surfaceMm, int32_t bedrockDepthMm, int64_t vz,
                                 bool& caveTruthOut) {
    uint32_t m = 0;
    caveTruthOut = caveCarveAt(cv.full, surfaceMm, bedrockDepthMm, vz);
    if (caveInShaftBranch(cv.full, surfaceMm, vz)) m |= 1u << CAVE_FAM_SHAFT;
    const bool tun = caveCarveAt(cv.tunnel, surfaceMm, bedrockDepthMm, vz);
    if (tun) m |= 1u << CAVE_FAM_TUNNEL;
    if (!tun && caveCarveAt(cv.tunCrev, surfaceMm, bedrockDepthMm, vz))
        m |= 1u << CAVE_FAM_CREVICE;
    if (cavernCarveAt(cavern, surfaceMm, bedrockDepthMm, vz)) m |= 1u << CAVE_FAM_CAVERN;
    return m;
}

// True if `mask` names any family the cave pass (not the cavern pass) owns --
// i.e. the set caveCarveAt is responsible for.
constexpr bool caveFamilyMaskIsCavePass(uint32_t mask) {
    return (mask & ((1u << CAVE_FAM_TUNNEL) | (1u << CAVE_FAM_CREVICE) | (1u << CAVE_FAM_SHAFT))) !=
           0;
}

// Single label for a voxel that belongs to several families; -1 if none.
constexpr int32_t caveDominantFamily(uint32_t mask) {
    for (uint32_t k = 0; k < kCaveFamilyCount; ++k)
        if (mask & (1u << kCaveFamilyPriority[k])) return static_cast<int32_t>(kCaveFamilyPriority[k]);
    return -1;
}

} // namespace vxc
