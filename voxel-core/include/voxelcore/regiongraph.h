#pragma once
// Hierarchical region-graph pathfinding (plan §3.6 NPCs & AI, M6): the
// single-level acceleration layer that lets HUNDREDS of NPC agents plan
// paths cheaply, sitting on top of pathfind.h's fine windowed voxel A* the
// same way an overworld map sits on top of turn-by-turn directions. Engine-
// free, integer-only, header-only, no terrain/UE dependency (doctrine §2,
// same "caller supplies a query over the region it cares about" shape as
// pathfind.h/connectivity.h). This header CONSUMES pathfind.h (PathCoord,
// MaterialFn, PathCostConfig, SearchWindow, findPath, detail::classifyMove)
// rather than reimplementing any of it — "the SAME walkability notion as
// pathfind.h" is enforced by literally calling into pathfind.h's classifier,
// not by a parallel copy that could drift.
//
// -----------------------------------------------------------------------
// Model (v0 — ONE abstraction level: regions + portals)
// -----------------------------------------------------------------------
// Space is partitioned into fixed-size cubic REGIONS of kRegionEdge=16
// voxels per side (power of two, so voxel<->region math is a floor-divide
// that a future optimization could turn into a shift/mask — kept as
// floorDiv/floorMod for readability in v0, matching core.h's existing
// floored-division convention rather than hand-rolled bit tricks). Each
// region's boundary with an axis-aligned neighbor (6 faces: PlusX, MinusX,
// PlusY, MinusY, PlusZ, MinusZ) may have zero or more PORTALS: a portal is
// one maximal 4-connected cluster of boundary cells where an agent can walk
// straight across that face (Walk/Mine for the four side faces, Climb/Fall
// for top/bottom — see "Simplifications" below for what's deliberately
// excluded). A portal is represented by its cluster's z-major-min cell
// (mirroring connectivity.h's Component::minCoord() convention) plus the
// cluster's cell count.
//
// Portals are MIRRORED per side of a boundary, not a single shared node: a
// doorway between region A (face PlusX) and region B (face MinusX) produces
// TWO portal nodes — one owned by A, one owned by B — connected by a cheap
// INTER-REGION edge (cost = the single-step crossing move's classifyMove
// cost, possibly asymmetric — see below). Nodes = portals. INTRA-REGION
// edges connect every pair of portals belonging to the SAME region, costed
// by a bounded fine findPath() confined to that region's own SearchWindow
// (skipped if unreachable within the region — no edge, not an infinite-cost
// edge). This is deliberately the mirrored-node design (not a single shared
// portal object per boundary) because it is what makes the incremental
// story below tractable: "region D's portals" is a well-defined, complete
// answer to "what could this edit have changed" without having to reason
// about which side of a boundary happens to "own" a merged node.
//
// -----------------------------------------------------------------------
// Why mirrored portals have IDENTICAL representative cells (load-bearing
// for inter-region edge matching, see detail::rebuildInterEdgesForFacePair)
// -----------------------------------------------------------------------
// Region A's PlusX-face open-mask at local (a,b) [a=local y, b=local z] is
// `classifyMove(cellA,+x).valid || classifyMove(cellB,-x).valid` where
// cellB = cellA+(1,0,0). Region B's (=A's +x neighbor) MinusX-face open-mask
// at the SAME (a,b) is `classifyMove(cellB,-x).valid ||
// classifyMove(cellA,+x).valid` — the identical boolean expression (OR is
// commutative), over the identical (y,z) domain (adjacent regions share
// their other two axes' extents exactly). Identical masks flood-filled by
// the identical deterministic algorithm produce identical cluster shapes
// and therefore identical representative-seed (a,b) positions on both
// sides. So region B's mirrored portal's cell is ALWAYS exactly region A's
// portal cell + faceNormal — inter-region edge construction looks this up
// directly (with a defensive fallback) instead of doing any geometric
// cluster-containment search.
//
// -----------------------------------------------------------------------
// Simplifications (v0 — documented, not built)
// -----------------------------------------------------------------------
// - Portal-forming moves are restricted to the PURE axis-aligned unit
//   offsets that correspond to a face crossing at the SAME (or, for top/
//   bottom faces, no) lateral position: Walk/Mine for the four side faces
//   (dz=0), Climb/Fall for top/bottom (dx=dy=0). StepUp/StepDown (which
//   cross a side face AND change z at once), Jump (2-voxel leap), and
//   Bridge (scaffold-dependent, not a static-world property) are NOT
//   portal-forming in v0 — they still work fine WITHIN a region's intra-
//   edge fine findPath (which uses the full 18-neighbor classifyMove
//   unrestricted), just never as the abstract inter-region hop itself. A
//   world that can only be crossed via a StepUp exactly on a region
//   boundary would (rarely, in practice — regions are 16 voxels wide)
//   under-report a portal there; documented, not solved, in v0.
// - ONE abstraction level. No multi-level nesting (super-regions of
//   regions) — later work, see plan §3.6 Tier 1/Tier 2.
// - No UE integration, no per-brick dirty-set plumbing (that's UE-side
//   glue over markRegionDirty, out of scope here — same relationship
//   pathfind.h's pathStillValid has to its future UE-side cache policy).
// - Portal ids are vector indices, tombstoned (Portal::alive=false) rather
//   than physically erased on recompute, so ids and edges stay valid
//   without an O(n) fix-up pass — but this means ids are stable ONLY within
//   one build/recompute cycle's lifetime, not guaranteed low/dense/compact.
//   Callers must not cache a portal id across a markRegionDirty call that
//   touches that portal's region (or its up-to-6 neighbors); re-resolve via
//   RegionGraph::portals scan (filtered by `alive`) after any mutation. No
//   compaction is implemented — a graph under heavy sustained dirty churn
//   accumulates dead entries; a full buildRegionGraph() rebuild reclaims
//   them (a caller-side policy decision, out of scope here).
// - Inter/intra edge costs are int32_t (mirrors PathCostConfig's weights);
//   an intra-region fine-path cost is clamped into int32_t range, which is
//   safe at region scale (kRegionEdge^3 cells x plausible per-step costs
//   stays far under INT32_MAX) but would need widening for a much larger
//   region edge.
//
// -----------------------------------------------------------------------
// Hierarchical query
// -----------------------------------------------------------------------
// findHierarchicalPath(graph, start, goal, solidFn, config, refine): locates
// start's and goal's regions; if they're the SAME region, skips the
// abstract graph entirely (one bounded fine findPath — no point abstracting
// a single-region hop). Otherwise: bounded fine findPath from `start` to
// every portal in its region (entry costs) and from every portal in goal's
// region to `goal` (exit costs), then a zero-heuristic Dijkstra over the
// portal graph (multi-source from the entry portals) with the SAME
// determinism discipline as pathfind.h's findPath — PortalKeyLess
// (region, then face, then cell — extending PathCoordLess's z-major
// convention to portals) is the ONLY priority-queue tie-break, deliberately
// keyed off (region,face,cell) rather than raw storage-order ids, so the
// search result does not depend on portal insertion/recompute order. If no
// entry portal can reach any exit portal, the query reports NOT FOUND —
// this header never falls back to an unbounded direct findPath (matches
// pathfind.h's own "never runs away over an unbounded world" doctrine one
// level up). `refine=true` additionally stitches a concrete PathResult:
// start->firstPortal and lastPortal->goal via fresh bounded fine findPath,
// each intra-region corridor hop via a fresh fine findPath (recomputed at
// query time, not cached from graph build, so a slightly-stale graph still
// yields a locally-correct refinement or an honest failure — it does NOT
// re-validate the ABSTRACT corridor itself against a changed solidFn; a
// caller worried about staleness should markRegionDirty the changed
// regions first), and each inter-region hop as a single synthesized step
// (classifyMove is O(1) — no findPath call needed for a single known
// crossing, which is exactly where the cheapness comes from).
//
// -----------------------------------------------------------------------
// Incremental dirtying
// -----------------------------------------------------------------------
// markRegionDirty(graph, region, solidFn, config) recomputes ONLY: (1)
// `region`'s own portals (all 6 faces — a voxel edit anywhere inside the
// region can in principle change any of its faces) and intra-region edges;
// (2) for each of `region`'s up to 6 face-neighbors N that's in-bounds,
// N's portal cluster on ONLY the ONE face mirroring `region` (a change
// inside `region` cannot affect classifyMove results read for N's OTHER
// faces — those never consult a voxel inside `region`) and N's intra-region
// edges (recomputed in full for N, since its portal set on that one face
// may have gained/lost/moved portals); (3) inter-region edges on every
// boundary incident to `region`. Regions two or more hops from `region` are
// never touched — this is the "NOT the whole graph" primitive the plan
// asks for. See the header comment above ("Why mirrored portals...") for
// why this is provably sufficient: a voxel changed only inside `region` can
// only change classifyMove outcomes for (a) cell pairs with at least one
// cell inside `region`, and (b) the fine findPath confined to `region`'s
// own SearchWindow — nothing outside `region` ∪ neighbors(`region`) reads
// those voxels. test_regiongraph.cpp proves markRegionDirty's result
// digests byte-identical to a from-scratch buildRegionGraph() over the same
// post-edit solidFn.
//
// -----------------------------------------------------------------------
// Determinism
// -----------------------------------------------------------------------
// RegionGraph::digest() sorts a canonical COPY of the (alive) portal list
// by PortalKeyLess and resolves edge endpoints through that canonical rank
// rather than hashing raw storage ids/order — this is what makes
// markRegionDirty's tombstone-and-append storage strategy (which does NOT
// preserve original vector indices/order) still produce a byte-identical
// digest to a freshly-built graph: only the semantic (region,face,cell)
// identity of each portal and the resolved (fromKey,toKey,cost,kind) triple
// of each edge is hashed, in a sorted, storage-order-independent sequence.

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include "voxelcore/core.h"
#include "voxelcore/pathfind.h"

namespace vxc {

// Region edge length in voxels — power of two, fixed at compile time (see
// header comment "Model"). 16 voxels: small enough that intra-region
// O(portals^2) fine findPath calls stay cheap (a region face is rarely more
// than a handful of portals), large enough that a multi-hundred-voxel world
// collapses to a few dozen regions per axis.
inline constexpr int64_t kRegionEdge = 16;
inline constexpr int64_t kRegionEdgeShift = 4;
static_assert((int64_t{1} << kRegionEdgeShift) == kRegionEdge,
              "kRegionEdgeShift must be log2(kRegionEdge)");
inline constexpr int64_t kRegionVolume = kRegionEdge * kRegionEdge * kRegionEdge;

// Region-space coordinate (NOT a voxel coordinate — see regionOfVoxel).
struct RegionCoord {
    int64_t x = 0, y = 0, z = 0;
    friend bool operator==(const RegionCoord&, const RegionCoord&) = default;
};

// Deterministic total order (z-major, then y, then x), mirroring
// VoxelCoordLess/PathCoordLess elsewhere in voxel-core.
struct RegionCoordLess {
    bool operator()(const RegionCoord& a, const RegionCoord& b) const {
        if (a.z != b.z) return a.z < b.z;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    }
};

inline RegionCoord regionOfVoxel(PathCoord v) {
    return RegionCoord{floorDiv(v.x, kRegionEdge), floorDiv(v.y, kRegionEdge), floorDiv(v.z, kRegionEdge)};
}

inline PathCoord regionMinCorner(RegionCoord r) {
    return PathCoord{r.x * kRegionEdge, r.y * kRegionEdge, r.z * kRegionEdge};
}

inline PathCoord regionMaxCorner(RegionCoord r) {
    const PathCoord lo = regionMinCorner(r);
    return PathCoord{lo.x + kRegionEdge - 1, lo.y + kRegionEdge - 1, lo.z + kRegionEdge - 1};
}

// A fine SearchWindow confined to exactly one region's voxel box — the
// bound every intra-region fine findPath() call in this header uses.
inline SearchWindow regionWindow(RegionCoord r, int32_t maxExpansions) {
    return SearchWindow{regionMinCorner(r), regionMaxCorner(r), maxExpansions};
}

inline bool regionInBounds(RegionCoord r, RegionCoord lo, RegionCoord hi) {
    return r.x >= lo.x && r.x <= hi.x && r.y >= lo.y && r.y <= hi.y && r.z >= lo.z && r.z <= hi.z;
}

// One of a region's 6 faces. Fixed enumeration order (not load-bearing for
// correctness, only for a stable digest/iteration order — same rationale
// as pathfind.h's kNeighborOffsets).
enum class Face : uint8_t { PlusX = 0, MinusX = 1, PlusY = 2, MinusY = 3, PlusZ = 4, MinusZ = 5 };

inline constexpr std::array<Face, 6> kAllFaces = {Face::PlusX, Face::MinusX, Face::PlusY,
                                                   Face::MinusY, Face::PlusZ, Face::MinusZ};

inline constexpr std::array<int64_t, 3> faceNormal(Face f) {
    switch (f) {
        case Face::PlusX: return {1, 0, 0};
        case Face::MinusX: return {-1, 0, 0};
        case Face::PlusY: return {0, 1, 0};
        case Face::MinusY: return {0, -1, 0};
        case Face::PlusZ: return {0, 0, 1};
        case Face::MinusZ: return {0, 0, -1};
    }
    return {0, 0, 0};
}

inline constexpr Face mirrorFace(Face f) {
    switch (f) {
        case Face::PlusX: return Face::MinusX;
        case Face::MinusX: return Face::PlusX;
        case Face::PlusY: return Face::MinusY;
        case Face::MinusY: return Face::PlusY;
        case Face::PlusZ: return Face::MinusZ;
        case Face::MinusZ: return Face::PlusZ;
    }
    return Face::PlusX;
}

inline RegionCoord neighborRegion(RegionCoord r, Face f) {
    const std::array<int64_t, 3> n = faceNormal(f);
    return RegionCoord{r.x + n[0], r.y + n[1], r.z + n[2]};
}

// One portal node: a maximal 4-connected cluster of crossable boundary
// cells on ONE region's ONE face (see header comment "Model" for why
// mirrored boundaries produce two Portal objects, not one shared node).
// `cell` is the cluster's z-major-min cell (PathCoordLess), on THIS
// portal's own `region` side of the boundary.
struct Portal {
    uint32_t id = 0;
    RegionCoord region;
    Face face = Face::PlusX;
    PathCoord cell;
    uint32_t clusterSize = 0;
    bool alive = true; // tombstoned by markRegionDirty rather than erased — see header comment
};

// Deterministic total order over portals by (region, face, cell) — the
// canonical identity digest()/findHierarchicalPath's Dijkstra tie-break use
// instead of storage-order ids (see header comment "Determinism").
struct PortalKeyLess {
    bool operator()(const Portal& a, const Portal& b) const {
        if (!(a.region == b.region)) return RegionCoordLess{}(a.region, b.region);
        if (a.face != b.face) return static_cast<uint8_t>(a.face) < static_cast<uint8_t>(b.face);
        return PathCoordLess{}(a.cell, b.cell);
    }
};

// One directed edge between two portal nodes (portal ids into
// RegionGraph::portals). `intraRegion=true` -> a bounded fine findPath
// result confined to one region's SearchWindow; `intraRegion=false` -> a
// single-step face crossing (cost = that classifyMove's cost). Costs may
// differ per direction (asymmetric, exactly like pathfind.h's own action
// model — e.g. Mine one way, trivial Walk back). `alive=false` marks an
// edge tombstoned by a markRegionDirty recompute (see header comment).
struct PortalEdge {
    uint32_t from = 0;
    uint32_t to = 0;
    int32_t cost = 0;
    bool intraRegion = true;
    bool alive = true;
};

// The built abstraction: portals (nodes) + directed edges, bounded to
// [minRegion, maxRegion] (inclusive, region-space — the caller-chosen
// analysis box, matching SearchWindow's "caller supplies a bounded region"
// doctrine one level up). `totalFinePathExpansions` is a running counter of
// every fine findPath() expansion spent building/maintaining this graph
// (build + every markRegionDirty call) — a monitoring/benchmarking number,
// deliberately excluded from digest() (it is not structural graph data).
struct RegionGraph {
    RegionCoord minRegion, maxRegion;
    std::vector<Portal> portals;
    std::vector<PortalEdge> edges;
    int64_t totalFinePathExpansions = 0;

    // Deterministic digest over the ALIVE portals/edges, in canonical
    // (PortalKeyLess-sorted) order — see header comment "Determinism".
    // Regression/golden-test primitive, mirroring PathResult::digest /
    // ConnectivityResult::digest (core.h's Digest, FNV-1a 64).
    void digest(Digest& d) const {
        d.i64(minRegion.x); d.i64(minRegion.y); d.i64(minRegion.z);
        d.i64(maxRegion.x); d.i64(maxRegion.y); d.i64(maxRegion.z);

        std::vector<uint32_t> order;
        order.reserve(portals.size());
        for (size_t i = 0; i < portals.size(); ++i)
            if (portals[i].alive) order.push_back(static_cast<uint32_t>(i));
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return PortalKeyLess{}(portals[a], portals[b]);
        });

        std::vector<uint32_t> rank(portals.size(), 0);
        for (size_t r = 0; r < order.size(); ++r) rank[order[r]] = static_cast<uint32_t>(r);

        d.u32(static_cast<uint32_t>(order.size()));
        for (uint32_t idx : order) {
            const Portal& p = portals[idx];
            d.i64(p.region.x); d.i64(p.region.y); d.i64(p.region.z);
            d.u8(static_cast<uint8_t>(p.face));
            d.i64(p.cell.x); d.i64(p.cell.y); d.i64(p.cell.z);
            d.u32(p.clusterSize);
        }

        struct EdgeKey {
            uint32_t from, to;
            int32_t cost;
            uint8_t intra;
        };
        std::vector<EdgeKey> ek;
        ek.reserve(edges.size());
        for (const PortalEdge& e : edges) {
            if (!e.alive || !portals[e.from].alive || !portals[e.to].alive) continue;
            ek.push_back({rank[e.from], rank[e.to], e.cost, e.intraRegion ? uint8_t(1) : uint8_t(0)});
        }
        std::sort(ek.begin(), ek.end(), [](const EdgeKey& a, const EdgeKey& b) {
            if (a.from != b.from) return a.from < b.from;
            if (a.to != b.to) return a.to < b.to;
            if (a.intra != b.intra) return a.intra < b.intra;
            return a.cost < b.cost;
        });
        d.u32(static_cast<uint32_t>(ek.size()));
        for (const EdgeKey& e : ek) {
            d.u32(e.from); d.u32(e.to); d.i64(static_cast<int64_t>(e.cost)); d.u8(e.intra);
        }
    }
};

namespace detail {

// One detected portal cluster, in region-local terms (converted to a
// world-space Portal by the caller).
struct PortalLocal {
    PathCoord cell;
    uint32_t clusterSize = 0;
};

// Maps a face's two free axes to a face-local (a,b) grid position, chosen
// so that iterating b outer / a inner ALWAYS matches global PathCoordLess
// order (z-major, then y, then x) regardless of which face this is — see
// header comment "Model" / connectivity.h's identical seed-is-minimum
// argument, reused here for portal representative-cell selection.
inline PathCoord faceLocalToCell(RegionCoord region, Face face, int64_t a, int64_t b) {
    const PathCoord lo = regionMinCorner(region);
    const PathCoord hi = regionMaxCorner(region);
    switch (face) {
        case Face::PlusX: return PathCoord{hi.x, lo.y + a, lo.z + b};
        case Face::MinusX: return PathCoord{lo.x, lo.y + a, lo.z + b};
        case Face::PlusY: return PathCoord{lo.x + a, hi.y, lo.z + b};
        case Face::MinusY: return PathCoord{lo.x + a, lo.y, lo.z + b};
        case Face::PlusZ: return PathCoord{lo.x + a, lo.y + b, hi.z};
        case Face::MinusZ: return PathCoord{lo.x + a, lo.y + b, lo.z};
    }
    return lo;
}

// classifyMove purely classifies a DESTINATION given a presumed-passable
// `from` — it never itself checks that `from` is non-solid (findPath's
// search never calls it on a solid `from` because it only ever expands
// from cells already reached as valid destinations of a prior move). Any
// caller that invokes classifyMove on an arbitrary boundary cell — as
// portal detection and inter-region edge construction below both do — must
// apply this same guard explicitly, or a wall's own solid interior cells
// would spuriously "classify" a move back out of themselves into open air
// as valid.
inline bool isAir(const MaterialFn& solidFn, PathCoord c) { return solidFn(c.x, c.y, c.z) == MAT_AIR; }

// Detects the portal clusters on one region's one face: a boundary cell
// pair (cell, nb=cell+faceNormal) is "open" only if BOTH cells are already
// air (see isAir above — a v0 simplification: the abstract portal graph
// represents CURRENTLY traversable openings, not "could become a doorway
// if someone mined it"; a wall that gets mined open becomes a portal via
// markRegionDirty once the edit lands, not pre-emptively) AND at least one
// direction's classifyMove is valid (an asymmetric doorway, e.g. a one-way
// drop, still counts — see header comment "Why mirrored portals have
// IDENTICAL representative cells": this OR is symmetric in cell/nb, which
// is what guarantees mirrored faces compute byte-identical open-masks).
// Open cells are 4-connected flood-filled in the face-local (a,b) grid;
// scan order = discovery order = PathCoordLess order (connectivity.h's
// seed-is-minimum argument).
inline std::vector<PortalLocal> findFacePortals(const MaterialFn& solidFn, const PathCostConfig& config,
                                                  RegionCoord region, Face face) {
    std::vector<PortalLocal> result;
    const size_t edge = static_cast<size_t>(kRegionEdge);
    const std::array<int64_t, 3> n = faceNormal(face);

    std::vector<uint8_t> open(edge * edge, 0);
    auto openIdx = [](int64_t a, int64_t b) {
        return static_cast<size_t>(a) + static_cast<size_t>(kRegionEdge) * static_cast<size_t>(b);
    };

    for (int64_t b = 0; b < kRegionEdge; ++b) {
        for (int64_t a = 0; a < kRegionEdge; ++a) {
            const PathCoord cell = faceLocalToCell(region, face, a, b);
            const PathCoord nb{cell.x + n[0], cell.y + n[1], cell.z + n[2]};
            bool isOpen = false;
            if (isAir(solidFn, cell) && isAir(solidFn, nb)) {
                const bool fwd = classifyMove(solidFn, config, cell, n[0], n[1], n[2]).valid;
                const bool back = classifyMove(solidFn, config, nb, -n[0], -n[1], -n[2]).valid;
                isOpen = fwd || back;
            }
            open[openIdx(a, b)] = isOpen ? 1 : 0;
        }
    }

    std::vector<uint8_t> visited(edge * edge, 0);
    std::vector<std::pair<int64_t, int64_t>> stack;
    for (int64_t b = 0; b < kRegionEdge; ++b) {
        for (int64_t a = 0; a < kRegionEdge; ++a) {
            const size_t startIdx = openIdx(a, b);
            if (!open[startIdx] || visited[startIdx]) continue;

            PortalLocal portal;
            portal.cell = faceLocalToCell(region, face, a, b); // seed == cluster min (see comment)
            portal.clusterSize = 0;
            visited[startIdx] = 1;
            stack.clear();
            stack.push_back({a, b});
            while (!stack.empty()) {
                const auto [ca, cb] = stack.back();
                stack.pop_back();
                ++portal.clusterSize;
                static constexpr std::array<std::array<int64_t, 2>, 4> kStep = {
                    {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
                for (const auto& s : kStep) {
                    const int64_t na = ca + s[0];
                    const int64_t nb2 = cb + s[1];
                    if (na < 0 || na >= kRegionEdge || nb2 < 0 || nb2 >= kRegionEdge) continue;
                    const size_t nIdx = openIdx(na, nb2);
                    if (!open[nIdx] || visited[nIdx]) continue;
                    visited[nIdx] = 1;
                    stack.push_back({na, nb2});
                }
            }
            result.push_back(portal);
        }
    }
    return result;
}

// Appends fresh Portal entries for one region+face's detected clusters
// (used by both buildRegionGraph and markRegionDirty).
inline void appendFacePortals(RegionGraph& graph, const MaterialFn& solidFn, const PathCostConfig& config,
                               RegionCoord region, Face face) {
    for (const PortalLocal& pl : findFacePortals(solidFn, config, region, face)) {
        Portal p;
        p.id = static_cast<uint32_t>(graph.portals.size());
        p.region = region;
        p.face = face;
        p.cell = pl.cell;
        p.clusterSize = pl.clusterSize;
        p.alive = true;
        graph.portals.push_back(p);
    }
}

// Tombstones every existing intra-region edge fully inside `region`, then
// appends a fresh set over the region's current ALIVE portals (bounded fine
// findPath, confined to `region`'s own SearchWindow — skipped if
// unreachable). O(portals(region)^2) fine findPath calls.
inline void rebuildIntraEdgesForRegion(RegionGraph& graph, RegionCoord region, const MaterialFn& solidFn,
                                        const PathCostConfig& config, int32_t capExpansions) {
    for (PortalEdge& e : graph.edges) {
        if (!e.intraRegion) continue;
        if (graph.portals[e.from].region == region && graph.portals[e.to].region == region) e.alive = false;
    }

    std::vector<uint32_t> ids;
    for (size_t i = 0; i < graph.portals.size(); ++i)
        if (graph.portals[i].alive && graph.portals[i].region == region) ids.push_back(static_cast<uint32_t>(i));

    const SearchWindow win = regionWindow(region, capExpansions);
    for (uint32_t i : ids) {
        for (uint32_t j : ids) {
            if (i == j) continue;
            const PathResult pr =
                findPath(solidFn, graph.portals[i].cell, graph.portals[j].cell, config, win);
            graph.totalFinePathExpansions += pr.expansionsUsed;
            if (!pr.complete) continue;
            PortalEdge e;
            e.from = i;
            e.to = j;
            e.cost = static_cast<int32_t>(pr.totalCost);
            e.intraRegion = true;
            e.alive = true;
            graph.edges.push_back(e);
        }
    }
}

// Tombstones every existing inter-region edge on the boundary between
// `regionA` (face `faceA`) and its neighbor across that face, then appends
// a fresh set by matching each of regionA's alive portals on `faceA` to its
// mirrored counterpart (identical cell + faceNormal — see header comment)
// and adding whichever direction(s) classifyMove actually permits.
inline void rebuildInterEdgesForFacePair(RegionGraph& graph, RegionCoord regionA, Face faceA,
                                          const MaterialFn& solidFn, const PathCostConfig& config) {
    const RegionCoord regionB = neighborRegion(regionA, faceA);
    const Face faceB = mirrorFace(faceA);

    for (PortalEdge& e : graph.edges) {
        if (e.intraRegion) continue;
        const Portal& pf = graph.portals[e.from];
        const Portal& pt = graph.portals[e.to];
        const bool aToB = pf.region == regionA && pf.face == faceA && pt.region == regionB && pt.face == faceB;
        const bool bToA = pf.region == regionB && pf.face == faceB && pt.region == regionA && pt.face == faceA;
        if (aToB || bToA) e.alive = false;
    }

    std::vector<uint32_t> idsA, idsB;
    for (size_t i = 0; i < graph.portals.size(); ++i) {
        const Portal& p = graph.portals[i];
        if (!p.alive) continue;
        if (p.region == regionA && p.face == faceA) idsA.push_back(static_cast<uint32_t>(i));
        if (p.region == regionB && p.face == faceB) idsB.push_back(static_cast<uint32_t>(i));
    }

    const std::array<int64_t, 3> n = faceNormal(faceA);
    for (uint32_t ia : idsA) {
        const PathCoord cellA = graph.portals[ia].cell;
        const PathCoord cellB{cellA.x + n[0], cellA.y + n[1], cellA.z + n[2]};
        uint32_t foundB = UINT32_MAX;
        for (uint32_t ib : idsB) {
            if (graph.portals[ib].cell == cellB) { foundB = ib; break; }
        }
        if (foundB == UINT32_MAX) continue; // defensive: proven not to happen, see header comment

        const MoveClassification mvFwd = classifyMove(solidFn, config, cellA, n[0], n[1], n[2]);
        if (mvFwd.valid) {
            PortalEdge e;
            e.from = ia; e.to = foundB; e.cost = mvFwd.cost; e.intraRegion = false; e.alive = true;
            graph.edges.push_back(e);
        }
        const MoveClassification mvBack = classifyMove(solidFn, config, cellB, -n[0], -n[1], -n[2]);
        if (mvBack.valid) {
            PortalEdge e;
            e.from = foundB; e.to = ia; e.cost = mvBack.cost; e.intraRegion = false; e.alive = true;
            graph.edges.push_back(e);
        }
    }
}

} // namespace detail

// Builds a RegionGraph over the inclusive region-space box [minRegion,
// maxRegion]. `intraRegionMaxExpansions<=0` defaults to kRegionVolume (a
// cap that can never spuriously truncate an in-region search — the region
// box has exactly that many cells). See header comment "Model".
inline RegionGraph buildRegionGraph(const MaterialFn& solidFn, RegionCoord minRegion, RegionCoord maxRegion,
                                     const PathCostConfig& config, int32_t intraRegionMaxExpansions = 0) {
    RegionGraph graph;
    graph.minRegion = minRegion;
    graph.maxRegion = maxRegion;
    if (maxRegion.x < minRegion.x || maxRegion.y < minRegion.y || maxRegion.z < minRegion.z) return graph;

    const int32_t cap =
        intraRegionMaxExpansions > 0 ? intraRegionMaxExpansions : static_cast<int32_t>(kRegionVolume);

    for (int64_t rz = minRegion.z; rz <= maxRegion.z; ++rz)
        for (int64_t ry = minRegion.y; ry <= maxRegion.y; ++ry)
            for (int64_t rx = minRegion.x; rx <= maxRegion.x; ++rx) {
                const RegionCoord region{rx, ry, rz};
                for (Face f : kAllFaces) {
                    if (!regionInBounds(neighborRegion(region, f), minRegion, maxRegion)) continue;
                    detail::appendFacePortals(graph, solidFn, config, region, f);
                }
            }

    for (int64_t rz = minRegion.z; rz <= maxRegion.z; ++rz)
        for (int64_t ry = minRegion.y; ry <= maxRegion.y; ++ry)
            for (int64_t rx = minRegion.x; rx <= maxRegion.x; ++rx)
                detail::rebuildIntraEdgesForRegion(graph, RegionCoord{rx, ry, rz}, solidFn, config, cap);

    // Each boundary visited exactly once: from the lower region via its
    // positive-direction faces only (the neighbor's mirrored negative face
    // is handled inside rebuildInterEdgesForFacePair itself).
    static constexpr std::array<Face, 3> kPositiveFaces = {Face::PlusX, Face::PlusY, Face::PlusZ};
    for (int64_t rz = minRegion.z; rz <= maxRegion.z; ++rz)
        for (int64_t ry = minRegion.y; ry <= maxRegion.y; ++ry)
            for (int64_t rx = minRegion.x; rx <= maxRegion.x; ++rx) {
                const RegionCoord region{rx, ry, rz};
                for (Face f : kPositiveFaces) {
                    if (!regionInBounds(neighborRegion(region, f), minRegion, maxRegion)) continue;
                    detail::rebuildInterEdgesForFacePair(graph, region, f, solidFn, config);
                }
            }

    return graph;
}

// Incremental recompute for a single dirtied region (e.g. after a dig
// touched a brick inside it): recomputes ONLY `region`'s portals/intra-
// edges, its up-to-6 neighbors' mirrored-face portal/intra-edges, and every
// inter-region edge incident to `region` — never the whole graph. See
// header comment "Incremental dirtying" for the correctness argument.
// No-op if `region` is outside the graph's own bounds.
inline void markRegionDirty(RegionGraph& graph, RegionCoord region, const MaterialFn& solidFn,
                             const PathCostConfig& config, int32_t intraRegionMaxExpansions = 0) {
    if (!regionInBounds(region, graph.minRegion, graph.maxRegion)) return;
    const int32_t cap =
        intraRegionMaxExpansions > 0 ? intraRegionMaxExpansions : static_cast<int32_t>(kRegionVolume);

    for (Portal& p : graph.portals)
        if (p.region == region) p.alive = false;
    for (Face f : kAllFaces) {
        if (!regionInBounds(neighborRegion(region, f), graph.minRegion, graph.maxRegion)) continue;
        detail::appendFacePortals(graph, solidFn, config, region, f);
    }

    for (Face f : kAllFaces) {
        const RegionCoord nb = neighborRegion(region, f);
        if (!regionInBounds(nb, graph.minRegion, graph.maxRegion)) continue;
        const Face mf = mirrorFace(f);
        for (Portal& p : graph.portals)
            if (p.region == nb && p.face == mf) p.alive = false;
        detail::appendFacePortals(graph, solidFn, config, nb, mf);
    }

    detail::rebuildIntraEdgesForRegion(graph, region, solidFn, config, cap);
    for (Face f : kAllFaces) {
        const RegionCoord nb = neighborRegion(region, f);
        if (!regionInBounds(nb, graph.minRegion, graph.maxRegion)) continue;
        detail::rebuildIntraEdgesForRegion(graph, nb, solidFn, config, cap);
    }

    for (Face f : kAllFaces) {
        if (!regionInBounds(neighborRegion(region, f), graph.minRegion, graph.maxRegion)) continue;
        detail::rebuildInterEdgesForFacePair(graph, region, f, solidFn, config);
    }
}

// The abstract result: an ordered chain of portal ids from the entry portal
// (in start's region) to the exit portal (in goal's region). Empty
// `portalIds` with `found=true` is the same-region special case (see
// findHierarchicalPath) — the whole trip never left one region.
struct RegionCorridor {
    std::vector<uint32_t> portalIds;
    int64_t totalCost = 0;
    bool found = false;
};

struct HierarchicalPathResult {
    PathCoord start, goal;
    RegionCorridor corridor;
    bool refined = false;
    PathResult concretePath; // valid iff refined
    // Fine-A* expansions spent on JUST the start-region entry search(es)
    // and goal-region exit search(es) — i.e. the cost of the corridor
    // query even when refine=false, since the Dijkstra step over the
    // portal graph itself touches zero fine-A* cells. This is the number
    // that stays roughly CONSTANT as start/goal grow further apart (each
    // additional region the corridor crosses adds O(1) portal-graph hops,
    // not another bounded region search) — the strongest form of the
    // "cheaper than a world-spanning search" claim; concretePath.
    // expansionsUsed (only meaningful when refined) additionally counts
    // every intermediate corridor hop's fine findPath and is the more
    // conservative "fully refined to a concrete step path" number.
    int64_t entryExitExpansionsUsed = 0;
};

// Locates start's/goal's regions, runs a zero-heuristic multi-source
// Dijkstra over the portal graph (PortalKeyLess tie-break — see header
// comment "Determinism"), and optionally refines to a concrete step path.
// Returns corridor.found=false (never an unbounded direct findPath) if
// start/goal fall outside the graph's bounds, either region has zero alive
// portals, or the two regions are disconnected in the abstract graph — see
// header comment "Hierarchical query".
inline HierarchicalPathResult findHierarchicalPath(const RegionGraph& graph, PathCoord start, PathCoord goal,
                                                    const MaterialFn& solidFn, const PathCostConfig& config,
                                                    bool refine = false, int32_t perRegionMaxExpansions = 0) {
    HierarchicalPathResult result;
    result.start = start;
    result.goal = goal;

    const RegionCoord rs = regionOfVoxel(start);
    const RegionCoord rg = regionOfVoxel(goal);
    if (!regionInBounds(rs, graph.minRegion, graph.maxRegion) ||
        !regionInBounds(rg, graph.minRegion, graph.maxRegion))
        return result;

    const int32_t cap =
        perRegionMaxExpansions > 0 ? perRegionMaxExpansions : static_cast<int32_t>(kRegionVolume);

    if (rs == rg) {
        const PathResult pr = findPath(solidFn, start, goal, config, regionWindow(rs, cap));
        result.corridor.found = pr.complete;
        result.corridor.totalCost = pr.totalCost;
        if (pr.complete && refine) {
            result.refined = true;
            result.concretePath = pr;
        }
        return result;
    }

    std::vector<uint32_t> startPortals, goalPortals;
    for (size_t i = 0; i < graph.portals.size(); ++i) {
        if (!graph.portals[i].alive) continue;
        if (graph.portals[i].region == rs) startPortals.push_back(static_cast<uint32_t>(i));
        if (graph.portals[i].region == rg) goalPortals.push_back(static_cast<uint32_t>(i));
    }
    if (startPortals.empty() || goalPortals.empty()) return result;

    struct Leg {
        uint32_t portal;
        int64_t cost;
        PathResult path;
    };
    std::vector<Leg> entries, exits;
    const SearchWindow rsWin = regionWindow(rs, cap);
    for (uint32_t pid : startPortals) {
        PathResult pr = findPath(solidFn, start, graph.portals[pid].cell, config, rsWin);
        if (pr.complete) entries.push_back({pid, pr.totalCost, pr});
    }
    if (entries.empty()) return result;

    const SearchWindow rgWin = regionWindow(rg, cap);
    for (uint32_t pid : goalPortals) {
        PathResult pr = findPath(solidFn, graph.portals[pid].cell, goal, config, rgWin);
        if (pr.complete) exits.push_back({pid, pr.totalCost, pr});
    }
    if (exits.empty()) return result;

    for (const Leg& e : entries) result.entryExitExpansionsUsed += e.path.expansionsUsed;
    for (const Leg& e : exits) result.entryExitExpansionsUsed += e.path.expansionsUsed;

    std::vector<std::vector<std::pair<uint32_t, int32_t>>> adj(graph.portals.size());
    for (const PortalEdge& e : graph.edges) {
        if (!e.alive || !graph.portals[e.from].alive || !graph.portals[e.to].alive) continue;
        adj[e.from].push_back({e.to, e.cost});
    }

    struct QEntry {
        int64_t g;
        uint32_t portal;
    };
    struct QCmp {
        const RegionGraph* graph;
        bool operator()(const QEntry& a, const QEntry& b) const {
            if (a.g != b.g) return a.g > b.g;
            return PortalKeyLess{}(graph->portals[b.portal], graph->portals[a.portal]);
        }
    };

    const size_t n = graph.portals.size();
    std::vector<int64_t> bestG(n, INT64_MAX);
    std::vector<uint32_t> parent(n, UINT32_MAX);
    std::vector<uint8_t> hasParent(n, 0);
    std::vector<uint8_t> settled(n, 0);
    std::priority_queue<QEntry, std::vector<QEntry>, QCmp> open{QCmp{&graph}};
    for (const Leg& e : entries) {
        if (e.cost < bestG[e.portal]) {
            bestG[e.portal] = e.cost;
            open.push({e.cost, e.portal});
        }
    }

    while (!open.empty()) {
        const QEntry cur = open.top();
        open.pop();
        if (settled[cur.portal]) continue;
        if (cur.g > bestG[cur.portal]) continue;
        settled[cur.portal] = 1;
        for (const auto& [to, cost] : adj[cur.portal]) {
            const int64_t ng = cur.g + cost;
            if (ng < bestG[to]) {
                bestG[to] = ng;
                hasParent[to] = 1;
                parent[to] = cur.portal;
                open.push({ng, to});
            }
        }
    }

    bool any = false;
    int64_t bestTotal = 0;
    uint32_t bestExit = 0;
    for (const Leg& ex : exits) {
        if (bestG[ex.portal] == INT64_MAX) continue;
        const int64_t total = bestG[ex.portal] + ex.cost;
        if (!any || total < bestTotal ||
            (total == bestTotal && PortalKeyLess{}(graph.portals[ex.portal], graph.portals[bestExit]))) {
            any = true;
            bestTotal = total;
            bestExit = ex.portal;
        }
    }
    if (!any) return result; // regions disconnected in the abstract graph -- no fallback (doctrine)

    std::vector<uint32_t> chain;
    uint32_t cur = bestExit;
    while (hasParent[cur]) {
        chain.push_back(cur);
        cur = parent[cur];
    }
    chain.push_back(cur);
    std::reverse(chain.begin(), chain.end());

    result.corridor.portalIds = chain;
    result.corridor.totalCost = bestTotal;
    result.corridor.found = true;

    if (refine) {
        PathResult full;
        full.start = start;
        full.goal = goal;
        full.reached = start;
        full.complete = true;
        full.capped = false;
        bool ok = true;

        auto appendSeg = [&](const PathResult& seg) {
            for (const PathStep& s : seg.steps) full.steps.push_back(s);
            full.totalCost += seg.totalCost;
            full.reached = seg.reached;
            full.expansionsUsed += seg.expansionsUsed;
        };

        for (const Leg& e : entries)
            if (e.portal == chain.front()) { appendSeg(e.path); break; }

        for (size_t i = 0; ok && i + 1 < chain.size(); ++i) {
            const Portal& pa = graph.portals[chain[i]];
            const Portal& pb = graph.portals[chain[i + 1]];
            if (pa.region == pb.region) {
                const PathResult seg = findPath(solidFn, pa.cell, pb.cell, config, regionWindow(pa.region, cap));
                if (!seg.complete) { ok = false; break; }
                appendSeg(seg);
            } else {
                const int64_t dx = pb.cell.x - pa.cell.x;
                const int64_t dy = pb.cell.y - pa.cell.y;
                const int64_t dz = pb.cell.z - pa.cell.z;
                const detail::MoveClassification mv = detail::classifyMove(solidFn, config, pa.cell, dx, dy, dz);
                if (!mv.valid) { ok = false; break; }
                PathStep st;
                st.cell = mv.to;
                st.action = mv.action;
                st.affectedCell = mv.affected;
                st.stepCost = mv.cost;
                full.steps.push_back(st);
                full.totalCost += mv.cost;
                full.reached = mv.to;
            }
        }

        if (ok)
            for (const Leg& e : exits)
                if (e.portal == chain.back()) { appendSeg(e.path); break; }

        if (ok) {
            result.refined = true;
            result.concretePath = full;
        }
    }

    return result;
}

} // namespace vxc
