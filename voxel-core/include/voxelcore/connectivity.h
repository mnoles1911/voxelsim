#pragma once
// Connectivity flood-fill (plan §3.5 Destruction & voxel bodies): "Connectivity
// flood-fill over affected region on structural edits -> disconnected islands
// promoted to rigid voxel debris bodies." This header is the M5-groundwork CPU
// reference for that flood-fill — engine-free, integer-only, no terrain/UE
// dependency (doctrine §2, matching waterca.h's shape: the caller supplies a
// solidity predicate over the region it already knows is affected, this file
// only implements the graph algorithm).
//
// -----------------------------------------------------------------------
// Scope
// -----------------------------------------------------------------------
// findComponents() partitions the solid voxels of a caller-given inclusive
// box [minCorner, maxCorner] into 6-connected (face-adjacent only, NOT
// 26-connected — diagonal/edge/corner touches do not connect) components.
// The box IS the analysis region: a component's extent is clipped to the
// box exactly like the "region around an edit" use case in the plan — a
// voxel just outside the box is never consulted, even if it would be solid
// and would join two in-box components together. Callers that need a wider
// blast radius simply pass a wider box; this header has no opinion on how
// big "the affected region" should be (that is a M5-in-UE policy decision).
//
// findDisconnectedIslands() is the concrete M5 use case: given the region
// touched by a structural edit and an anchorFn marking which voxels count as
// "grounded" (touches the world below the edit, attached to bedrock, nailed
// to a wall — caller's policy; the common case is "touches the region's
// bottom face", see bottomFaceAnchor() below), it returns which components
// have zero anchored voxels. Those are the floating islands that should be
// promoted to rigid voxel debris bodies; every anchored component stays part
// of the standing structure.
//
// -----------------------------------------------------------------------
// Determinism
// -----------------------------------------------------------------------
// Two independent guarantees, both required so a replayed edit log produces
// byte-identical debris-promotion decisions on every client (doctrine §2.3):
//   1. Component ORDER: components are ordered by their minimum voxel coord
//      under VoxelCoordLess (z-major, then y, then x — the same total order
//      BrickKeyLess uses for bricks). This falls out of the algorithm for
//      free rather than needing an explicit sort-by-component-min pass:
//      findComponents scans the box in VoxelCoordLess order (z outer, y
//      middle, x inner) and starts a new flood fill at the first unvisited
//      solid voxel it meets. Because every voxel ordered before that seed
//      has already been visited (either not solid, or claimed by an
//      earlier, lower-ordered component), the seed IS that component's
//      minimum voxel — scan order and "ordered by min voxel" coincide.
//   2. Voxel order WITHIN a component: sorted by VoxelCoordLess after the
//      flood fill completes, independent of BFS traversal/queue order (so a
//      future parallel/GPU flood fill that visits neighbors in a different
//      order still produces an identical component listing).
// ConnectivityResult::digest() folds both into one FNV-1a value (core.h's
// Digest) for regression/golden tests, mirroring Brick<B>::digest and
// WaterCA::digest.
//
// -----------------------------------------------------------------------
// Complexity / representation
// -----------------------------------------------------------------------
// O(volume) time and space: a dense solid-mask + visited byte array sized to
// the box's voxel count (box volume, not world size) is filled once by
// calling solidFn exactly once per voxel, then BFS'd with an index-based
// queue (no repeat solidFn calls, no per-neighbor hashmap lookups) — see the
// perf-sanity test in test_connectivity.cpp for a 64^3-region timing sample.
// This is a reference implementation for a caller-bounded "region around an
// edit" (plan §3.5), not meant to be called with a world-spanning box; a
// dense array over an unbounded region would defeat the point of the sparse
// voxel storage the rest of voxel-core uses.
//
// Component voxels are stored as a flat std::vector<VoxelCoord> (plain
// world-space integer coords) rather than a brick/palette-style compact
// encoding: a debris island is expected to be small (single tree, single
// wall section) relative to a whole chunk, so the flat list is simplest and
// the digest/order guarantees above are what downstream code (M5-in-UE
// debris promotion) actually needs, not compactness.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "voxelcore/core.h"

namespace vxc {

// Plain world-space voxel coordinate (not a brick key — this is a single
// voxel, brick.h's BrickKey is bricks-of-voxels).
struct VoxelCoord {
    int64_t x = 0, y = 0, z = 0;
    friend bool operator==(const VoxelCoord&, const VoxelCoord&) = default;
};

// Deterministic total order (z-major, then y, then x), mirroring
// BrickKeyLess (brick.h) at voxel granularity.
struct VoxelCoordLess {
    bool operator()(const VoxelCoord& a, const VoxelCoord& b) const {
        if (a.z != b.z) return a.z < b.z;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    }
};

// solidFn(vx, vy, vz): true if that world-space voxel is solid (occupied,
// non-air). Called at most once per voxel in the analysis box. Doctrine-clean
// — no MaterialId/terrain dependency; callers adapt (e.g.
// `[&](int64_t x,int64_t y,int64_t z){ return world.materialAt(x,y,z) !=
// MAT_AIR; }`).
using SolidFn = std::function<bool(int64_t vx, int64_t vy, int64_t vz)>;

// anchorFn(vx, vy, vz): true if that solid voxel counts as "grounded" —
// caller's policy (touches the region's bottom face, adjacent to
// never-edited bedrock, nailed to an unaffected wall brick, ...). Only ever
// called on voxels solidFn already returned true for.
using AnchorFn = std::function<bool(int64_t vx, int64_t vy, int64_t vz)>;

// One 6-connected component: every voxel reachable from any other voxel in
// the list via a chain of face-adjacent (dx+dy+dz == 1 in Manhattan terms)
// solid-voxel steps, all within the analysis box. Sorted by VoxelCoordLess
// (see header comment "Determinism").
struct Component {
    std::vector<VoxelCoord> voxels;

    size_t size() const { return voxels.size(); }

    // The component's minimum voxel under VoxelCoordLess — valid because
    // voxels is kept sorted; equal to the flood-fill seed by construction
    // (see header comment). Empty components never occur (findComponents
    // never emits a component with zero voxels).
    const VoxelCoord& minCoord() const { return voxels.front(); }
};

struct ConnectivityResult {
    int32_t componentCount = 0;
    std::vector<Component> components; // ordered by minCoord() (see header comment)

    // Deterministic digest over the ordered components and their ordered
    // voxels — regression/golden-test primitive, mirroring
    // Brick<B>::digest / WaterCA::digest (core.h's Digest, FNV-1a 64).
    void digest(Digest& d) const {
        d.u32(static_cast<uint32_t>(componentCount));
        for (const Component& c : components) {
            d.u32(static_cast<uint32_t>(c.voxels.size()));
            for (const VoxelCoord& v : c.voxels) {
                d.i64(v.x);
                d.i64(v.y);
                d.i64(v.z);
            }
        }
    }
};

// ODR / unity-build note (do NOT re-flatten this into `vxc::detail`):
// pathfind.h ALSO opens `namespace vxc::detail` and defines an inline
// `localIndex(int64_t,int64_t,int64_t,int64_t,int64_t)` with the identical
// signature. Two definitions of the same function in one translation unit is a
// hard redefinition error regardless of `inline` or `#pragma once` (those guard
// re-inclusion of the SAME file, not two different files declaring the same
// entity). That is exactly what UE's adaptive unity build produces: when
// VoxelWorldSubsystem.cpp (includes connectivity.h) is unmodified it rejoins
// the unity blob alongside VoxelAgentSubsystem.cpp/VoxelAgent.cpp (include
// pathfind.h), both headers land in one TU, and VoxelEarth fails to compile
// with a redefinition error pointing here. Giving each header its own nested
// detail namespace removes the collision for good; test_connectivity.cpp
// includes both headers in one TU as a standing regression guard.
namespace connectivity_detail {

// Local (box-relative) voxel index, x-fastest — matches Brick<B>::cellIndex's
// convention (x + edge*(y + edge*z)).
inline size_t localIndex(int64_t lx, int64_t ly, int64_t lz, int64_t dx, int64_t dy) {
    return static_cast<size_t>(lx) +
           static_cast<size_t>(dx) *
               (static_cast<size_t>(ly) + static_cast<size_t>(dy) * static_cast<size_t>(lz));
}

} // namespace connectivity_detail

// Flood-fills the solid voxels of the inclusive box [minCorner, maxCorner]
// into 6-connected components. Returns an empty result (componentCount == 0,
// no components) if the box is degenerate (any max < min). See header
// comment for ordering/determinism/complexity guarantees.
inline ConnectivityResult findComponents(const SolidFn& solidFn, VoxelCoord minCorner,
                                          VoxelCoord maxCorner) {
    ConnectivityResult result;
    if (maxCorner.x < minCorner.x || maxCorner.y < minCorner.y || maxCorner.z < minCorner.z)
        return result;

    const int64_t dx = maxCorner.x - minCorner.x + 1;
    const int64_t dy = maxCorner.y - minCorner.y + 1;
    const int64_t dz = maxCorner.z - minCorner.z + 1;
    const size_t volume = static_cast<size_t>(dx) * static_cast<size_t>(dy) * static_cast<size_t>(dz);

    // Precompute solidity once per voxel (avoids O(n) repeat solidFn calls
    // per voxel from neighbor checks during BFS).
    std::vector<uint8_t> solidMask(volume);
    for (int64_t lz = 0; lz < dz; ++lz)
        for (int64_t ly = 0; ly < dy; ++ly)
            for (int64_t lx = 0; lx < dx; ++lx) {
                const size_t idx = connectivity_detail::localIndex(lx, ly, lz, dx, dy);
                solidMask[idx] = solidFn(minCorner.x + lx, minCorner.y + ly, minCorner.z + lz) ? 1 : 0;
            }

    std::vector<uint8_t> visited(volume, 0);
    struct LocalCoord {
        int64_t lx, ly, lz;
    };
    std::vector<LocalCoord> queue;
    queue.reserve(volume);

    // Fixed 6-neighbor step order (face adjacency only — NOT the 26
    // diagonal/edge/corner neighbors): -x, +x, -y, +y, -z, +z. Named
    // kComponentFaceSteps rather than the obvious kComponentFaceSteps on purpose:
    // pathfind.h declares a NAMESPACE-SCOPE `vxc::kComponentFaceSteps`, and a
    // function-local of that name in a translation unit that also includes
    // pathfind.h trips MSVC's C4459 (local declaration hides global) — which UE
    // compiles as an ERROR under /W4 /WX, and UE's unity build routinely puts
    // both headers in one TU. Same family of hazard as the localIndex ODR
    // collision documented above, and the same fix: do not reuse a name another
    // voxel-core header has already claimed at namespace scope. The order
    // itself
    // itself is not load-bearing for the resulting component SET (any
    // order visits the same reachable voxels), only kept fixed for
    // readability/consistency with the rest of the codebase's fixed-order
    // conventions (e.g. waterca.h's gravity/+x/-x/+y/-y priority order).
    static constexpr std::array<std::array<int64_t, 3>, 6> kComponentFaceSteps = {{
        {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
    }};

    for (int64_t lz = 0; lz < dz; ++lz) {
        for (int64_t ly = 0; ly < dy; ++ly) {
            for (int64_t lx = 0; lx < dx; ++lx) {
                const size_t startIdx = connectivity_detail::localIndex(lx, ly, lz, dx, dy);
                if (!solidMask[startIdx] || visited[startIdx]) continue;

                Component comp;
                queue.clear();
                visited[startIdx] = 1;
                queue.push_back({lx, ly, lz});
                size_t head = 0;
                while (head < queue.size()) {
                    const LocalCoord c = queue[head++];
                    comp.voxels.push_back(
                        VoxelCoord{minCorner.x + c.lx, minCorner.y + c.ly, minCorner.z + c.lz});
                    for (const auto& off : kComponentFaceSteps) {
                        const int64_t nx = c.lx + off[0];
                        const int64_t ny = c.ly + off[1];
                        const int64_t nz = c.lz + off[2];
                        if (nx < 0 || nx >= dx || ny < 0 || ny >= dy || nz < 0 || nz >= dz) continue;
                        const size_t nIdx = connectivity_detail::localIndex(nx, ny, nz, dx, dy);
                        if (!solidMask[nIdx] || visited[nIdx]) continue;
                        visited[nIdx] = 1;
                        queue.push_back({nx, ny, nz});
                    }
                }

                std::sort(comp.voxels.begin(), comp.voxels.end(), VoxelCoordLess{});
                result.components.push_back(std::move(comp));
            }
        }
    }

    result.componentCount = static_cast<int32_t>(result.components.size());
    return result;
}

// The M5 use case: full component breakdown of the edit-affected region,
// split into which components are "grounded" (contain >=1 anchorFn-true
// voxel — stay part of the standing structure) and which are floating
// islands (zero anchored voxels — promote to rigid voxel debris). Indices
// index into connectivity.components (which retains the full
// findComponents() breakdown, including component order/digest, for callers
// that want it).
struct IslandAnalysis {
    ConnectivityResult connectivity;
    std::vector<int32_t> islandComponentIndices;   // no anchored voxel -> debris
    std::vector<int32_t> anchoredComponentIndices; // >=1 anchored voxel -> stays
};

inline IslandAnalysis findDisconnectedIslands(const SolidFn& solidFn, VoxelCoord minCorner,
                                               VoxelCoord maxCorner, const AnchorFn& anchorFn) {
    IslandAnalysis result;
    result.connectivity = findComponents(solidFn, minCorner, maxCorner);
    for (size_t i = 0; i < result.connectivity.components.size(); ++i) {
        const Component& c = result.connectivity.components[i];
        bool anchored = false;
        for (const VoxelCoord& v : c.voxels) {
            if (anchorFn(v.x, v.y, v.z)) {
                anchored = true;
                break;
            }
        }
        (anchored ? result.anchoredComponentIndices : result.islandComponentIndices)
            .push_back(static_cast<int32_t>(i));
    }
    return result;
}

// Convenience anchorFn factory for the common case named in the plan:
// "voxels touching the region's bottom face" are grounded. `minZ` is
// normally the analysis box's minCorner.z.
inline AnchorFn bottomFaceAnchor(int64_t minZ) {
    return [minZ](int64_t /*vx*/, int64_t /*vy*/, int64_t vz) { return vz == minZ; };
}

} // namespace vxc
