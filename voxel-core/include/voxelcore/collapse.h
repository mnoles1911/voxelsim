#pragma once
// Structural collapse for LARGE edits (plan §4 M5, "structural collapse") —
// the other half of connectivity.h. Engine-free, integer-only, deterministic,
// same "caller supplies a predicate over the region it cares about" shape as
// connectivity.h / waterca.h / pathfind.h.
//
// =======================================================================
// Why connectivity.h alone is not enough (the hole this file fills)
// =======================================================================
// connectivity.h answers "which pieces of this box are 6-connected to an
// anchored voxel". Its M5-in-UE caller anchors on "touches the analysis box's
// side or bottom face", which is only SOUND when the box fully CONTAINS the
// candidate piece with margin — a piece that runs out of the box is assumed to
// continue into the standing world and is never removed. That is the safe
// direction, but it means a box that clamps (a big explosive carve) can only
// answer "everything is anchored", so the UE caller had to skip detection
// entirely for large edits. Result: a 10m crater detached nothing and left
// impossible floating geometry.
//
// Making the box bigger does not fix it. Three things break at scale:
//   (a) a dense O(volume) voxel scan over a 50m³ blast is ~100M voxels;
//   (b) at any box size, "touches the boundary ⇒ anchored" is a statement
//       about the BOX, not about the world, so correctness still depends on
//       containment — you have only moved the cliff, not removed it;
//   (c) pure connectivity cannot express the case that actually matters for
//       explosives: a roof whose pillars were blown out but which still
//       incidentally touches one wall is "connected" and would never fall.
//
// =======================================================================
// The model here: DIFFERENTIAL COARSE SUPPORT
// =======================================================================
// Three ideas, each of which independently removes a false-positive mode.
//
// -- 1. Coarse cells (conservative in the SAFE direction) ----------------
// The analysis grid is COARSE CELLS, not voxels — one cell is a cube of
// `cellVoxels` voxels (the UE caller uses 8, i.e. exactly one brick, so cell
// occupancy is answerable straight off the brick occupancy bitset / the
// heightfield, with no per-voxel terrain evaluation). A cell is "occupied" if
// it contains AT LEAST ONE solid voxel.
//
// Coarse adjacency is a strict OVER-approximation of voxel adjacency: if two
// voxels are 6-connected through solid voxels, then their cells are
// 6-connected through occupied cells (project the voxel path onto cells). The
// converse is false — two voxels can share a cell, or be in adjacent occupied
// cells, without touching at all.
//
// The direction of that inequality is everything: coarsening can only ever
// MERGE things that are really separate, i.e. it can only ever make a piece
// look MORE attached than it is. So a coarse-unsupported piece is definitely
// unsupported; coarsening can MISS a collapse but can never INVENT one. That
// is the opposite of the failure mode we must avoid (wrongly deleting
// standing terrain), so the approximation is free.
//
// -- 2. Support = ground-reachability with a lateral budget --------------
// Not plain connectivity. Over the occupied cells, define
//
//     dist(c) = min over paths from the anchor set to c of
//               (number of LATERAL steps on the path)
//
// where a step to a face-adjacent occupied cell costs 1 if it is horizontal
// (±x, ±y) and 0 if it is vertical (±z). Anchors have dist 0. A cell is
// SUPPORTED iff dist(c) ≤ maxLateralCells.
//
// In words: **load travels up and down a stack of blocks for free, but only
// spans a bounded horizontal distance from whatever is carrying it.** A column
// standing on the ground is supported to any height (vertical is free). A slab
// cantilevered off that column is supported out to maxLateralCells cells and
// no further. A slab attached to nothing is supported nowhere.
//
// This is a shortest-path distance, so it is a unique function of the occupied
// set — NOT a function of traversal order. Any correct relaxation order
// produces byte-identical output (see "Determinism" below).
//
// Note there is deliberately NO budget "reset" on vertical steps. An earlier
// formulation reset the budget whenever a cell rested on a supported cell;
// that lets a 2-cell-thick slab zig-zag up/lateral/down/lateral and span an
// unbounded distance, which is exactly wrong. Plain "vertical is free, lateral
// costs 1, never reset" has no such loophole.
//
// -- 3. DIFFERENTIAL: only a CHANGE in support collapses ----------------
// The support model above, applied absolutely, would flag every natural
// terrain feature that overhangs further than the budget — arches, cliff
// undercuts, sea caves — and disintegrate them the first time anyone digs
// anywhere nearby. So support is evaluated TWICE over the same region, once
// against the pre-edit occupancy and once against the post-edit occupancy, and
//
//     a cell SEEDS a collapse iff
//         occupied-after ∧ supported-BEFORE ∧ ¬supported-AFTER
//
// i.e. only mass whose support THIS EDIT actually destroyed falls. Natural
// geology that was already unsupported under the model is grandfathered in and
// is never touched, no matter how many edits happen near it. Pre-edit
// occupancy is not a stored snapshot: for a removal-only edit it is exactly
// (post-edit occupancy ∪ the cells containing the just-cleared voxels), which
// the caller already has in hand.
//
// -- 3b. CLOSURE: a falling mass comes down whole ------------------------
// The seed rule alone leaves a wart. Take a roof that already overhung its
// pillar by more than the budget: its outer fringe was unsupported BEFORE the
// edit, so grandfathering protects it — and when the pillar is blown out and
// the roof's core falls, that fringe is left hanging in the air on its own.
// So the seed set is CLOSED under 6-connectivity through cells that are
// occupied-after and unsupported-after:
//
//     collapse = the set of occupied-after, ¬supported-after cells that are
//                6-connected to at least one seed cell
//
// Physically: whatever was holding this mass up is gone, and anything hanging
// off it that also has no support of its own goes with it. This cannot leak
// into standing terrain, because the flood only crosses ¬supported-after
// cells and all ground-connected mass is supported-after by construction. And
// it cannot un-grandfather isolated geology: a natural arch that touches
// nothing that is falling has no seed in its component and never moves.
//
// -- Why this is sound at ANY region size (the property the box lacked) --
// Anchors are the region's bottom and four side faces (never the top). Shrink
// or clamp the region and interior cells become boundary cells — which are
// anchored, hence dist 0, in BOTH the before pass and the after pass. A cell
// that is supported in both passes can never satisfy the collapse predicate.
// Therefore **clamping the region can only ever REMOVE collapse decisions,
// never add one.** Under-sizing the region degrades recall, never precision.
// The bounded box was unsound because its correctness depended on containing
// the piece; here nothing depends on containment, so the region size is a
// pure cost/recall knob and can be capped as hard as the frame budget likes.
//
// =======================================================================
// Determinism
// =======================================================================
//  * dist() is a shortest-path distance function: unique given the occupied
//    set, independent of the order cells are relaxed in. computeSupport()
//    uses a bucket queue (bucket k holds the cells at lateral distance k) and
//    test_collapse.cpp proves order-independence by feeding the same geometry
//    through permuted seed orders and comparing digests.
//  * Collapsing cells are emitted sorted by VoxelCoordLess (z-major, then y,
//    then x) — the same total order connectivity.h's components use.
//  * splitIntoComponents() sorts its input and seeds in VoxelCoordLess order,
//    so component ORDER and within-component voxel order are both fixed
//    regardless of BFS visit order.
//  * CollapseAnalysis::digest() folds the whole decision into one FNV-1a value
//    (core.h's Digest) for golden/regression pinning.
//  * Integer only — no float anywhere in this header (CI float-ban).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

#include "voxelcore/connectivity.h" // VoxelCoord, VoxelCoordLess, Component
#include "voxelcore/core.h"

namespace vxc {

// occupiedFn(cx, cy, cz): true if COARSE CELL (cx,cy,cz) contains at least one
// solid voxel. Cell (cx,cy,cz) covers voxels
// [cx*cellVoxels, (cx+1)*cellVoxels) × ... — but this header never needs
// cellVoxels: it works purely in cell coordinates and the caller owns the
// voxel↔cell mapping (for the UE caller a cell IS a brick, so the mapping is
// ChunkMap<B>::keyForVoxel and the query is a brick occupancy test).
// Called at most once per cell in the analysis region.
using CoarseOccupiedFn = std::function<bool(int64_t cx, int64_t cy, int64_t cz)>;

// Sentinel lateral distance for "no path from any anchor" (unreachable).
inline constexpr int32_t kUnsupportedDist = INT32_MAX;

// Support field over an inclusive cell-space box. `dist` is the lateral-step
// shortest-path distance defined in the header comment; `occupied` is the
// cached occupiedFn result (one call per cell).
struct SupportField {
    VoxelCoord minCell{}, maxCell{};
    int64_t dx = 0, dy = 0, dz = 0;   // cell extents
    std::vector<uint8_t> occupied;    // 0/1, x-fastest (see cellIndex)
    std::vector<int32_t> dist;        // kUnsupportedDist where unreachable
    int32_t maxLateralCells = 0;

    bool valid() const { return dx > 0 && dy > 0 && dz > 0; }

    // Local index of a cell given LOCAL (region-relative) coords, x-fastest —
    // same convention as Brick<B>::cellIndex.
    size_t localCellIndex(int64_t lx, int64_t ly, int64_t lz) const {
        return static_cast<size_t>(lx) +
               static_cast<size_t>(dx) *
                   (static_cast<size_t>(ly) + static_cast<size_t>(dy) * static_cast<size_t>(lz));
    }

    bool inRegion(int64_t cx, int64_t cy, int64_t cz) const {
        return cx >= minCell.x && cx <= maxCell.x && cy >= minCell.y && cy <= maxCell.y &&
               cz >= minCell.z && cz <= maxCell.z;
    }

    bool isOccupied(int64_t cx, int64_t cy, int64_t cz) const {
        if (!inRegion(cx, cy, cz)) return false;
        return occupied[localCellIndex(cx - minCell.x, cy - minCell.y, cz - minCell.z)] != 0;
    }

    int32_t distAt(int64_t cx, int64_t cy, int64_t cz) const {
        if (!inRegion(cx, cy, cz)) return kUnsupportedDist;
        return dist[localCellIndex(cx - minCell.x, cy - minCell.y, cz - minCell.z)];
    }

    // Supported == reachable from an anchor within the lateral budget.
    bool isSupported(int64_t cx, int64_t cy, int64_t cz) const {
        const int32_t d = distAt(cx, cy, cz);
        return d != kUnsupportedDist && d <= maxLateralCells;
    }

    int32_t occupiedCount() const {
        int32_t n = 0;
        for (uint8_t o : occupied) n += (o != 0) ? 1 : 0;
        return n;
    }
    int32_t supportedCount() const {
        int32_t n = 0;
        for (size_t i = 0; i < dist.size(); ++i)
            if (occupied[i] && dist[i] != kUnsupportedDist && dist[i] <= maxLateralCells) ++n;
        return n;
    }
};

// Computes the support field over the inclusive cell box [minCell, maxCell].
//
// Anchor set: every occupied cell on the region's BOTTOM face (cz == minCell.z)
// or on any of its four SIDE faces (cx/cy == min/max). NOT the top face — a
// piece that only reaches the top of the region is hanging in the air as far
// as this region can tell, and "reaches the top" must not mean "grounded".
// Anchoring the sides/bottom is the conservative reading of "this mass
// continues into the standing world outside the region", and is what makes
// clamping the region safe (see header comment).
//
// Returns an invalid (empty) field for a degenerate box.
inline SupportField computeSupport(const CoarseOccupiedFn& occupiedFn, VoxelCoord minCell,
                                   VoxelCoord maxCell, int32_t maxLateralCells) {
    SupportField f;
    if (maxCell.x < minCell.x || maxCell.y < minCell.y || maxCell.z < minCell.z) return f;
    if (maxLateralCells < 0) maxLateralCells = 0;

    f.minCell = minCell;
    f.maxCell = maxCell;
    f.dx = maxCell.x - minCell.x + 1;
    f.dy = maxCell.y - minCell.y + 1;
    f.dz = maxCell.z - minCell.z + 1;
    f.maxLateralCells = maxLateralCells;
    const size_t volume =
        static_cast<size_t>(f.dx) * static_cast<size_t>(f.dy) * static_cast<size_t>(f.dz);
    f.occupied.assign(volume, 0);
    f.dist.assign(volume, kUnsupportedDist);

    for (int64_t lz = 0; lz < f.dz; ++lz)
        for (int64_t ly = 0; ly < f.dy; ++ly)
            for (int64_t lx = 0; lx < f.dx; ++lx)
                f.occupied[f.localCellIndex(lx, ly, lz)] =
                    occupiedFn(minCell.x + lx, minCell.y + ly, minCell.z + lz) ? 1 : 0;

    // Bucket queue: bucket k holds cells whose lateral distance is exactly k.
    // Only distances 0..maxLateralCells can ever produce a supported cell, so
    // buckets beyond the budget are never created and cells past it keep
    // kUnsupportedDist (the field only needs to distinguish supported from
    // not; the exact distance of an unsupported cell is not used anywhere).
    const size_t bucketCount = static_cast<size_t>(maxLateralCells) + 1;
    std::vector<std::vector<size_t>> buckets(bucketCount);

    // Seed: occupied cells on the bottom / side faces, in VoxelCoordLess scan
    // order (z outer, y middle, x inner). Order is not load-bearing for the
    // RESULT (dist is a shortest-path distance, unique), only for
    // reproducible-by-inspection bucket contents.
    for (int64_t lz = 0; lz < f.dz; ++lz) {
        for (int64_t ly = 0; ly < f.dy; ++ly) {
            for (int64_t lx = 0; lx < f.dx; ++lx) {
                const bool onSideOrBottom =
                    (lz == 0) || (lx == 0) || (lx == f.dx - 1) || (ly == 0) || (ly == f.dy - 1);
                if (!onSideOrBottom) continue;
                const size_t idx = f.localCellIndex(lx, ly, lz);
                if (!f.occupied[idx] || f.dist[idx] == 0) continue;
                f.dist[idx] = 0;
                buckets[0].push_back(idx);
            }
        }
    }

    // Face-adjacent neighbour steps. Vertical (±z) costs 0 lateral steps,
    // horizontal (±x, ±y) costs 1. Fixed order for readability only.
    struct Step {
        int64_t ox, oy, oz;
        int32_t cost;
    };
    static constexpr std::array<Step, 6> kSteps = {{
        {-1, 0, 0, 1}, {1, 0, 0, 1}, {0, -1, 0, 1}, {0, 1, 0, 1}, {0, 0, -1, 0}, {0, 0, 1, 0},
    }};

    for (size_t k = 0; k < bucketCount; ++k) {
        // Indexed loop, not a range-for: the cost-0 (vertical) relaxations
        // below push into THIS same bucket while it is being drained, which is
        // what closes each bucket under free vertical movement before any
        // cost-1 step is taken. That closure is why the bucket queue computes
        // the true shortest-path distance (a 0-1 BFS in bucket form).
        for (size_t qi = 0; qi < buckets[k].size(); ++qi) {
            const size_t idx = buckets[k][qi];
            if (f.dist[idx] != static_cast<int32_t>(k)) continue; // stale entry
            const int64_t lx = static_cast<int64_t>(idx % static_cast<size_t>(f.dx));
            const int64_t ly =
                static_cast<int64_t>((idx / static_cast<size_t>(f.dx)) % static_cast<size_t>(f.dy));
            const int64_t lz = static_cast<int64_t>(idx / static_cast<size_t>(f.dx) /
                                                    static_cast<size_t>(f.dy));
            for (const Step& s : kSteps) {
                const int64_t nx = lx + s.ox, ny = ly + s.oy, nz = lz + s.oz;
                if (nx < 0 || nx >= f.dx || ny < 0 || ny >= f.dy || nz < 0 || nz >= f.dz) continue;
                const size_t nIdx = f.localCellIndex(nx, ny, nz);
                if (!f.occupied[nIdx]) continue;
                const int32_t nd = static_cast<int32_t>(k) + s.cost;
                if (nd > maxLateralCells) continue; // past the budget — leave unsupported
                if (f.dist[nIdx] <= nd) continue;
                f.dist[nIdx] = nd;
                buckets[static_cast<size_t>(nd)].push_back(nIdx);
            }
        }
    }

    return f;
}

// Tunables for one collapse evaluation. All integer; all documented in
// docs/status.md "Structural collapse (M5, large-edit)".
struct CollapseParams {
    // Cantilever budget in COARSE CELLS. With the UE caller's 8-voxel (0.8m)
    // cells, 6 cells = 4.8m of horizontal span from the nearest thing carrying
    // load. Larger = more conservative (fewer collapses).
    int32_t maxLateralCells = 6;
    // Hard ceiling on how many cells one edit may collapse. Past this the
    // analysis reports bTruncated and emits the first cells in VoxelCoordLess
    // order — deterministic, and a bounded amount of work/edit-log churn.
    int32_t maxCollapsingCells = 4096;
};

struct CollapseAnalysis {
    // Cells that collapse, sorted by VoxelCoordLess. The caller turns these
    // into the voxel set to remove (all solid-after voxels inside them).
    std::vector<VoxelCoord> collapsingCells;

    // Diagnostics (logged by the UE caller; also what the tests assert on).
    int32_t occupiedCellsAfter = 0;
    int32_t supportedCellsBefore = 0;
    int32_t supportedCellsAfter = 0;
    bool bTruncated = false; // hit maxCollapsingCells

    void digest(Digest& d) const {
        d.u32(static_cast<uint32_t>(collapsingCells.size()));
        for (const VoxelCoord& c : collapsingCells) {
            d.i64(c.x);
            d.i64(c.y);
            d.i64(c.z);
        }
        d.u32(static_cast<uint32_t>(occupiedCellsAfter));
        d.u32(static_cast<uint32_t>(supportedCellsBefore));
        d.u32(static_cast<uint32_t>(supportedCellsAfter));
        d.u32(bTruncated ? 1u : 0u);
    }
};

// THE ENTRY POINT. Runs computeSupport twice over the same cell region — once
// against pre-edit occupancy, once against post-edit occupancy — and returns
// the cells that are occupied after the edit, were supported before it, and
// are not supported after it (see header comment for why all three conjuncts
// are required).
//
// Both predicates are called at most once per cell.
inline CollapseAnalysis findCollapsingCells(const CoarseOccupiedFn& occupiedBeforeFn,
                                            const CoarseOccupiedFn& occupiedAfterFn,
                                            VoxelCoord minCell, VoxelCoord maxCell,
                                            const CollapseParams& params = {}) {
    CollapseAnalysis result;
    const SupportField before =
        computeSupport(occupiedBeforeFn, minCell, maxCell, params.maxLateralCells);
    const SupportField after =
        computeSupport(occupiedAfterFn, minCell, maxCell, params.maxLateralCells);
    if (!before.valid() || !after.valid()) return result;

    result.occupiedCellsAfter = after.occupiedCount();
    result.supportedCellsBefore = before.supportedCount();
    result.supportedCellsAfter = after.supportedCount();

    const size_t volume = after.occupied.size();
    const auto supportedIn = [&params](const SupportField& f, size_t i) {
        return f.occupied[i] != 0 && f.dist[i] != kUnsupportedDist &&
               f.dist[i] <= params.maxLateralCells;
    };
    // "Falling" = occupied after the edit and no longer supported. The seed
    // pass marks the subset of those whose support this edit actually removed;
    // the closure pass then floods through the rest (see header §3b).
    std::vector<uint8_t> falling(volume, 0);
    std::vector<size_t> queue;
    for (size_t i = 0; i < volume; ++i) {
        if (!after.occupied[i] || supportedIn(after, i)) continue;
        if (!supportedIn(before, i)) continue; // grandfathered geology — not a seed
        falling[i] = 1;
        queue.push_back(i);
    }
    // Seeds are pushed in local-index order, which IS VoxelCoordLess order
    // (x-fastest indexing, z outermost). Flood order does not affect the
    // RESULT anyway — the closure is a connected-component membership test,
    // not a distance — but keeping it fixed makes the pass reproducible by
    // inspection like the rest of voxel-core.
    // Face-adjacency steps. Named kClosureFaceSteps, not kNeighborOffsets:
    // pathfind.h declares a namespace-scope vxc::kNeighborOffsets and MSVC's
    // C4459 (local hides global) is an ERROR under UE's /W4 /WX whenever the
    // unity build lands both headers in one TU. See connectivity.h's note.
    static constexpr std::array<std::array<int64_t, 3>, 6> kClosureFaceSteps = {{
        {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
    }};
    for (size_t head = 0; head < queue.size(); ++head) {
        const size_t idx = queue[head];
        const int64_t lx = static_cast<int64_t>(idx % static_cast<size_t>(after.dx));
        const int64_t ly = static_cast<int64_t>((idx / static_cast<size_t>(after.dx)) %
                                                static_cast<size_t>(after.dy));
        const int64_t lz =
            static_cast<int64_t>(idx / static_cast<size_t>(after.dx) / static_cast<size_t>(after.dy));
        for (const auto& off : kClosureFaceSteps) {
            const int64_t nx = lx + off[0], ny = ly + off[1], nz = lz + off[2];
            if (nx < 0 || nx >= after.dx || ny < 0 || ny >= after.dy || nz < 0 || nz >= after.dz)
                continue;
            const size_t nIdx = after.localCellIndex(nx, ny, nz);
            if (falling[nIdx] || !after.occupied[nIdx] || supportedIn(after, nIdx)) continue;
            falling[nIdx] = 1;
            queue.push_back(nIdx);
        }
    }

    // Emit in VoxelCoordLess order (z outer, y middle, x inner) so
    // collapsingCells comes out sorted with no separate sort pass, and so the
    // maxCollapsingCells truncation keeps a deterministic prefix.
    for (int64_t lz = 0; lz < after.dz; ++lz) {
        for (int64_t ly = 0; ly < after.dy; ++ly) {
            for (int64_t lx = 0; lx < after.dx; ++lx) {
                const size_t idx = after.localCellIndex(lx, ly, lz);
                if (!falling[idx]) continue;
                if (static_cast<int32_t>(result.collapsingCells.size()) >= params.maxCollapsingCells) {
                    result.bTruncated = true;
                    return result;
                }
                result.collapsingCells.push_back(
                    VoxelCoord{minCell.x + lx, minCell.y + ly, minCell.z + lz});
            }
        }
    }
    return result;
}

namespace collapse_detail {

struct VoxelCoordHash {
    size_t operator()(const VoxelCoord& v) const {
        // splitmix-style fold of the three axes; matches the "mix the axes with
        // hash64 then combine" habit of hash.h rather than a naive xor.
        uint64_t h = 1469598103934665603ULL;
        auto mix = [&h](int64_t v2) {
            h ^= static_cast<uint64_t>(v2);
            h *= 1099511628211ULL;
            h ^= h >> 29;
        };
        mix(v.x);
        mix(v.y);
        mix(v.z);
        return static_cast<size_t>(h);
    }
};

} // namespace collapse_detail

// Splits an arbitrary SET of solid voxels (not a box) into 6-connected
// components. Used on the collapsing voxel set, which is a sparse, possibly
// very large, arbitrarily-shaped blob — a dense mask over its bounding box (as
// findComponents uses) would be the wrong representation there, so this is a
// hash-set BFS instead: O(n) expected, memory proportional to the SET, not to
// its bounding box.
//
// Determinism: the input is sorted by VoxelCoordLess first, seeds are taken in
// that order (so component order is by minimum voxel, exactly like
// findComponents), and each component's voxel list is sorted after its BFS
// completes (so traversal order cannot leak into the output).
// Duplicate input coords are collapsed.
inline std::vector<Component> splitIntoComponents(std::vector<VoxelCoord> voxels) {
    std::vector<Component> out;
    if (voxels.empty()) return out;

    std::sort(voxels.begin(), voxels.end(), VoxelCoordLess{});
    voxels.erase(std::unique(voxels.begin(), voxels.end()), voxels.end());

    std::unordered_set<VoxelCoord, collapse_detail::VoxelCoordHash> remaining;
    remaining.reserve(voxels.size() * 2);
    for (const VoxelCoord& v : voxels) remaining.insert(v);

    static constexpr std::array<std::array<int64_t, 3>, 6> kSplitFaceSteps = {{
        {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
    }};

    std::vector<VoxelCoord> queue;
    for (const VoxelCoord& seed : voxels) {
        if (remaining.find(seed) == remaining.end()) continue; // already claimed
        Component comp;
        queue.clear();
        remaining.erase(seed);
        queue.push_back(seed);
        size_t head = 0;
        while (head < queue.size()) {
            const VoxelCoord c = queue[head++];
            comp.voxels.push_back(c);
            for (const auto& off : kSplitFaceSteps) {
                const VoxelCoord n{c.x + off[0], c.y + off[1], c.z + off[2]};
                const auto it = remaining.find(n);
                if (it == remaining.end()) continue;
                remaining.erase(it);
                queue.push_back(n);
            }
        }
        std::sort(comp.voxels.begin(), comp.voxels.end(), VoxelCoordLess{});
        out.push_back(std::move(comp));
    }
    return out;
}

// Digest over a component list (collapse pieces), for golden/regression tests
// and for the UE side's "same edit ⇒ same collapse decision" assertion.
inline void digestComponents(const std::vector<Component>& comps, Digest& d) {
    d.u32(static_cast<uint32_t>(comps.size()));
    for (const Component& c : comps) {
        d.u32(static_cast<uint32_t>(c.voxels.size()));
        for (const VoxelCoord& v : c.voxels) {
            d.i64(v.x);
            d.i64(v.y);
            d.i64(v.z);
        }
    }
}

} // namespace vxc
