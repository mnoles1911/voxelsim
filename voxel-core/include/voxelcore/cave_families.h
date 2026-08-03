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

// v27: the crevice GATE is no longer a bit of crevHash (it moved to its own
// channel, caves.h CH_CAVE_CREV_GATE), so the tunnel-only variant closes the
// lattice edge's `crevOpen` flag directly instead of poisoning a hash. Same
// property, one indirection fewer: the tunnel emission never reads either.
inline constexpr uint64_t kCaveCrevHashClosed = 1ull << 61;

struct CaveColumnVariants {
    CaveColumn full;    // tunnels + crevices + shaft (== ColumnSample::cave)
    CaveColumn tunnel;  // tunnels only
    CaveColumn tunCrev; // tunnels + crevices, no shaft
};

// `full` is bit-identical to what caveColumnFor(seed, vx, vy, surfaceMm)
// returns, including its below-threshold empty case -- callers cross-check
// that against the ColumnSample they already have.
// THE v24 ENTRANCE, OUT OF THE v25 WORLD -- the control panel for W3's A/B.
//
// An A/B that shows a feature is PRESENT needs the frame where it is absent,
// and the absent frame has to differ in exactly one thing. Building the actual
// v24 tree gets that wrong twice over: it also reverts nothing-to-do-with-W3
// (and on the fine tier the v24 instrument cannot even load the tiles), and it
// makes the comparison a two-build argument instead of a one-world one.
//
// So the control is made the same way the family attribution is: by
// DIFFERENCING the shipping predicate over a controlled variant of the lattice
// block. Zeroing `entranceReachMm` makes the cavity's own xy reject
// (distSq < reachSq) fail at every column, which leaves exactly the THROAT --
// a bore of the same hashed radius from the surface to the same node. That IS
// v24's entrance construct, in the same world, with everything else identical.
// No worldgen is edited and nothing here is mirrored in the shader.
inline CaveLattice caveLatticeWithoutEntranceCavity(CaveLattice L) {
    L.entranceReachMm = 0;
    return L;
}

// THE v25 CHAMBER, OUT OF THE v26 WORLD -- the control panel for W4's A/B,
// and the reason W4's symmetry statistic is a measurement rather than an
// assertion.
//
// WHY A CONTROL IS NOT OPTIONAL HERE. "Plan-view symmetry is visibly broken"
// is a claim about a NUMBER going up, and a number that goes up proves
// nothing unless the same number can be shown to come out LOW on geometry
// that really is symmetric. Any plan-view asymmetry statistic has three ways
// to read high for reasons that have nothing to do with the chamber: the roof
// clamp truncates a room against a slope into a crescent; the bedrock clamp
// cuts its bottom off; the sampled region clips it at the edge. All three
// were already true at v25. Measuring one arm and reporting "asymmetric"
// would therefore have been the W3 mouth-metric mistake again -- a statistic
// satisfied by geometry other than the one under test.
//
// Differencing removes all three at once, because both arms see the SAME
// terrain, the same clamps, the same region and the same sites, and differ in
// exactly the four terms W4 added. The v25 value is the control that proves
// the statistic can report "symmetric" at all.
//
// Built the same way the entrance control is: by neutralising fields of the
// shipping struct, not by editing worldgen and not by building a v25 tree.
// Nothing here is mirrored in the shader and nothing here can move a digest.
//
// NOT neutralised, deliberately: kCavernRzDeepMinMm's v26 rise from 12 m to
// 16 m. That is a room SIZE change, not a shape one -- it makes deep rooms
// taller, which no symmetry statistic reads -- and reverting it here would
// make the two arms differ in something the A/B is not about.
inline CavernSite cavernSiteWithoutChamberShape(CavernSite s) {
    for (int32_t c = 0; c < kCavernChildCount; ++c) {
        s.children[c].xMm = s.anchorXMm; // coaxial again: no leaning chain
        s.children[c].yMm = s.anchorYMm;
        s.children[c].dirCosQ12 = static_cast<int32_t>(kCavernDirOne); // round in plan again
        s.children[c].dirSinQ12 = 0;
        s.children[c].elongQ10 = 1024;
    }
    s.pillarRadiusMm = 0; // no pillars
    s.breakdownAmpMm = 0; // flat machined floors
    return s;
}

// The v25-shaped cavern column for a site out of the v26 world. Goes through
// the shipping reduction (`cavernColumnFromSites`), so the roughness sample,
// the reach reject, the segment cap and `cavernCarveAt` are all the real
// ones -- only the site's shape fields differ.
template <typename SurfaceFn>
inline CavernColumn cavernColumnWithoutChamberShape(uint64_t seed, const CavernCandidates& cands,
                                                    int64_t vx, int64_t vy,
                                                    const SurfaceFn& surfaceAt) {
    return cavernColumnFromSites(
        seed, cands, vx, vy, [&](int64_t fi, int64_t fj, const CaveNode& node) {
            return cavernSiteWithoutChamberShape(cavernSiteFor(seed, fi, fj, node, surfaceAt));
        });
}

// THE v26 FIELD COUPLING — i.e. NONE — OUT OF THE v27 WORLD. The control panel
// for W5's A/B, and the reason every W5 number is a difference.
//
// WHY IT HAS TO BE A DIFFERENCE, in the same shape as W3's and W4's. "Mountains
// have more and bigger caves than desert plains" is a claim about numbers
// moving in opposite directions at two places. Measuring the shipping arm at
// two places and reporting the ratio would be satisfied by things that are not
// the coupling at all: a mountain has more SOLID BAND per column to carve into
// (its surface is higher above the bedrock band and above sea level), its
// steeper ground truncates entrances differently, and its columns clip the
// sample region differently. All three were already true at v26, when the cave
// pass read nothing about the place.
//
// So both arms are reduced on the SAME columns, the same terrain, the same
// seed, the same shipping predicate, and differ in exactly one thing: whether
// the six gate values come from the field or from kCaveGatesNeutral. Neutral is
// not a fixture value — it is v26's rates, and for the edge and entrance gates
// it is v26's exact draws (caves.h caveGateOpen explains how a threshold
// reproduces a two-bit test). The crevice gate is rate-identical only, because
// its channel moved; that one exception is stated at CH_CAVE_CREV_GATE rather
// than left for someone to find.
//
// Nothing here edits worldgen, nothing here is mirrored in the shader, and
// nothing here can move a digest.
template <typename SurfaceFn>
inline CaveLattice caveLatticeWithoutFieldCoupling(uint64_t seed, int64_t ci, int64_t cj,
                                                   const SurfaceFn& surfaceAt) {
    return caveLatticeForGates(seed, ci, cj, surfaceAt,
                               [](int64_t, int64_t) { return kCaveGatesNeutral; });
}

template <typename SurfaceFn, typename FieldFn>
inline CaveColumnVariants caveColumnVariantsFor(uint64_t seed, int64_t vx, int64_t vy,
                                                int32_t surfaceMm, const SurfaceFn& surfaceAt,
                                                const FieldFn& fieldAt,
                                                bool entranceCavityOff = false,
                                                bool fieldCouplingOff = false) {
    CaveColumnVariants out;
    if (surfaceMm < kCaveMinSurfaceMm) return out; // caveColumnFor's own guard
    const int64_t ci = floorDiv(vx * kVoxelSizeMm, kCaveLatticeMm);
    const int64_t cj = floorDiv(vy * kVoxelSizeMm, kCaveLatticeMm);
    const CaveLattice base = fieldCouplingOff
                                 ? caveLatticeWithoutFieldCoupling(seed, ci, cj, surfaceAt)
                                 : caveLatticeFor(seed, ci, cj, surfaceAt, fieldAt);
    const CaveLattice L =
        entranceCavityOff ? caveLatticeWithoutEntranceCavity(base) : base;

    CaveLattice lns = L;
    lns.shaftNodeSlot = -1;
    CaveLattice lt = lns;
    for (CaveLatticeEdge& e : lt.edges) {
        e.crevHash = kCaveCrevHashClosed;
        e.crevOpen = false;
    }

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
