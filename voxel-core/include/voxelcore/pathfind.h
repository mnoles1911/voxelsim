#pragma once
// Dig-aware windowed voxel A* (plan §3.6 NPCs & AI): "local windowed voxel
// A* where traversing air is cheap, digging costs (hardness x time), placing
// scaffold blocks has cost -> walking, mining, tunneling, bridging fall out
// of ONE cost function. Incremental nav invalidation per dirty brick. Do NOT
// use stock UE NavMesh as primary." This header is the M6-groundwork CPU
// reference for that cost function and search — engine-free, integer-only,
// no terrain/UE dependency (doctrine §2, matching connectivity.h/waterca.h's
// shape: the caller supplies a material query over the region it already
// knows is relevant, this file only implements the graph algorithm). The
// region-graph hierarchical layer, UE agent integration, and real per-dirty-
// brick cache invalidation policy are M6-proper, later — see
// `pathStillValid` below for the recheck primitive that policy will use.
//
// -----------------------------------------------------------------------
// Body model (v0)
// -----------------------------------------------------------------------
// The agent is a single lattice voxel: it occupies exactly one cell
// (vx,vy,vz) at a time and has no separate head-clearance requirement (a
// future 2-voxel-tall body is out of scope for v0 — document, don't build).
// "Standing" means the agent's current cell is non-solid (air) and, for
// every action EXCEPT the two vertical ones (Climb/Fall) and Mine, the cell
// directly below the agent's DESTINATION must be solid for the move to
// count as a supported step (see action model below); Climb/Fall move along
// a column without a support check (a shaft or open drop), and Mine simply
// requires the destination itself to be excavated (support at the far end
// is not re-verified — mining always breaches straight into a neighbor
// cell, matching "digging costs hardness x time" without extra bookkeeping).
//
// -----------------------------------------------------------------------
// Neighbor model
// -----------------------------------------------------------------------
// 18 fixed candidate offsets from a cell, in four groups (kNeighborOffsets
// below, in a FIXED order — not load-bearing for the resulting optimal path,
// same "fixed for readability/consistency" rationale as connectivity.h's
// neighbor order, since determinism actually comes from the priority queue's
// tie-break, not offset iteration order):
//   1. 6-CONNECTED CORE (the classic face-adjacent 6): flat horizontal
//      +-x/+-y (dz=0) and pure vertical +-z (dx=dy=0). This is exactly
//      connectivity.h's 6-connectivity notion, reused at the "can an agent
//      step or climb this way" level instead of "are these two voxels the
//      same rigid body".
//   2. STEP UP / STEP DOWN (8): one horizontal axis moved +-1 combined with
//      one vertical voxel of rise or drop (dz=+-1) — climbing or descending
//      a single-voxel ledge in one action instead of two.
//   3. JUMP (4): a 2-voxel horizontal leap (dx=+-2 xor dy=+-2, dz=0) over a
//      1-voxel-wide gap, using PathCostConfig::jumpGapCost — the cheap
//      alternative to Bridge for a gap exactly one voxel wide (see the
//      action model's Jump case for the "does not require a placed block"
//      distinction).
//
// -----------------------------------------------------------------------
// Action model — the ONE cost function
// -----------------------------------------------------------------------
// Every candidate neighbor is classified by detail::classifyMove into
// exactly one of 8 actions, purely as a function of (origin, offset,
// solidFn, config) — walking, mining, tunneling, and bridging are NOT
// separate algorithms, they are what this single classification produces
// for different world/config inputs at the SAME call site:
//   Walk     — flat move (dz=0), destination air, destination supported
//              (solid immediately below). Cost: config.walkCost.
//   StepUp   — step move with dz=+1, destination air, supported. Cost:
//              config.stepUpCost.
//   StepDown — step move with dz=-1, destination air, supported. Cost:
//              config.walkCost (a controlled single-ledge descent onto
//              solid footing is priced like a normal walk step, distinct
//              from an open-air Fall — see below). An UNSUPPORTED step-down
//              is not a valid move in v0 (no action fits it cleanly); the
//              search instead finds it as a Fall chained after a flat/step
//              move that already crossed the ledge, decomposing a diagonal
//              drop into its horizontal and vertical parts.
//   Climb    — pure vertical (dx=dy=0), dz=+1, destination air. No support
//              check (climbing a shaft). Cost: config.stepUpCost (reused —
//              moving up one voxel costs the same tunable whether the move
//              is a horizontal step-up or a straight vertical climb).
//   Fall     — pure vertical, dz=-1, destination air. No support check
//              (falling through open air; a multi-voxel drop is simply a
//              chain of single-voxel Fall actions, so "per voxel" in
//              config.fallCostPerVoxel falls out of chaining, not a
//              separate multi-voxel-drop code path). Cost:
//              config.fallCostPerVoxel.
//   Mine     — destination is SOLID and not MAT_BEDROCK, and
//              config.mineCostByMaterial[destMat] >= 0 (a negative entry is
//              the caller's "this material is impassable" sentinel).
//              Applies uniformly across flat/step/vertical offsets — mining
//              is "the same move, but the destination happens to be solid",
//              never a separate movement rule. Cost:
//              config.mineCostByMaterial[destMat]. MAT_BEDROCK is HARD-CODED
//              impassable regardless of config (plan: "bedrock = effectively
//              infinite/unmineable") — a caller cannot accidentally make it
//              mineable by misconfiguring the cost table.
//   Bridge   — flat or step-up move where the destination is air but NOT
//              supported: the agent "places" a scaffold block at the cell
//              directly below the destination (PathStep::affectedCell,
//              distinct from PathStep::cell) and steps onto it. Cost:
//              config.bridgeCost.
//   Jump     — 2-voxel horizontal leap (see neighbor model group 3): valid
//              only when the 1-voxel midpoint is air (nothing to breach)
//              AND the midpoint is UNSUPPORTED (no solid cell directly
//              below it — a genuine gap, not just ordinary walkable ground;
//              otherwise Jump would silently undercut Walk/StepUp any time
//              jumpGapCost < 2x their cost, over perfectly normal terrain)
//              AND the destination is air AND supported. No block is
//              placed (PathStep::affectedCell == PathStep::cell) — this is
//              what distinguishes Jump from Bridge for a gap exactly one
//              voxel wide: Bridge spends a placed block to walk it in two
//              single-voxel steps, Jump spends config.jumpGapCost to cross
//              it in one leap with nothing placed. Whichever is cheaper
//              under the caller's config wins, same as every other
//              walk/mine/bridge tradeoff in this header.
//
// This is the concrete "walking, mining, tunneling, bridging fall out of
// ONE cost function" proof the plan names: test_pathfind.cpp runs the exact
// same findPath() against the exact same world with only PathCostConfig's
// weights changed and shows the chosen action sequence flips (tunnel
// straight through a rock wall vs. detour around it; bridge a gap vs. detour
// around/down it) purely from those weights.
//
// mineCostByMaterial defaults to all-zero (every non-bedrock material free
// to mine) via PathCostConfig's default member initializer — a deliberate
// "caller must configure real hardness costs" contract, not a usable
// default; leaving an entry at 0 means "free to mine", not "impassable" (use
// a negative value for that, or rely on the hard-coded MAT_BEDROCK case).
//
// -----------------------------------------------------------------------
// Search / windowing
// -----------------------------------------------------------------------
// findPath runs Dijkstra (A* with a always-admissible zero heuristic),
// bounded to a caller-given inclusive box (SearchWindow::minCorner/
// maxCorner) plus an expansion cap (SearchWindow::maxExpansions): only cells
// inside the box are ever pushed onto the open set or settled, and the
// search stops (capped=true) the moment either the box's reachable region is
// exhausted or the cap is hit, whichever comes first — it never runs away
// over an unbounded world. A zero heuristic is deliberate, not a missed
// optimization: an admissible heuristic would have to stay valid across
// arbitrary caller-supplied PathCostConfig weights (including near-zero
// mine/bridge costs a caller might legitimately want to test), and getting
// that wrong would silently break optimality exactly when it matters most;
// zero-heuristic (uniform-cost search) guarantees the returned path is
// truly cheapest under whatever config was passed, at the cost of exploring
// more nodes — a cost the window/cap already bounds. Node bookkeeping
// (best-cost, parent, settled) is a dense array sized to the window's voxel
// volume, indexed the same x-fastest way as connectivity.h's detail::
// localIndex, not a hashmap — same "reference impl over a caller-bounded
// region" tradeoff connectivity.h documents, not meant for a world-spanning
// box.
//
// When the goal is not reached (box exhausted or cap hit), the result is
// still useful "best-effort toward goal": PathResult reconstructs the path
// to whichever settled node had the smallest Manhattan distance to the goal
// (ties broken by lower cost, then PathCoordLess) instead of an empty
// result.
//
// -----------------------------------------------------------------------
// Determinism
// -----------------------------------------------------------------------
// The open-set priority queue's ONLY tie-break, when two entries have equal
// cost, is PathCoordLess (z-major, then y, then x — the same total order
// VoxelCoordLess/BrickKeyLess use elsewhere in voxel-core): see QueueCmp
// below. Combined with the fixed kNeighborOffsets iteration order and pure
// (no time/thread/hash-iteration dependence) classification logic, the same
// (solidFn, start, goal, config, window) always produces the byte-identical
// step sequence and PathResult::digest() value. test_pathfind.cpp pins a
// golden digest.
//
// -----------------------------------------------------------------------
// Incremental invalidation (stub — M6-proper policy is later, in UE)
// -----------------------------------------------------------------------
// pathStillValid(path, solidFn, config) re-classifies every step of an
// already-computed path against a (possibly changed) solidFn and returns
// false the instant any step's action no longer matches what was recorded —
// a dirty brick that changed a path's cells (a wall appeared, a mined
// tunnel got refilled, bedrock got exposed) is caught this way without
// re-running the search. A future incremental cache (M6-proper, UE-side)
// would keep PathResults keyed by (start, goal, configHash) and, whenever a
// brick's dirty-set is known, call this on cached results whose steps touch
// that brick — only those get invalidated/recomputed, not the whole cache.
// This header intentionally stops at the recheck primitive; the caching and
// per-brick indexing policy is out of scope here (matches connectivity.h's
// "this header has no opinion on how big the affected region should be").

#include <array>
#include <cstdint>
#include <functional>
#include <queue>
#include <vector>

#include "voxelcore/core.h"

namespace vxc {

// Plain world-space voxel coordinate (int64_t per docs/determinism.md's
// "World voxel coords" convention) — deliberately its own type rather than
// reusing connectivity.h's VoxelCoord, so this header stays a standalone
// module with no cross-file coupling (same "each domain header defines its
// own coordinate type" precedent connectivity.h itself set relative to
// brick.h's BrickKey).
struct PathCoord {
    int64_t x = 0, y = 0, z = 0;
    friend bool operator==(const PathCoord&, const PathCoord&) = default;
};

// Deterministic total order (z-major, then y, then x), mirroring
// VoxelCoordLess/BrickKeyLess.
struct PathCoordLess {
    bool operator()(const PathCoord& a, const PathCoord& b) const {
        if (a.z != b.z) return a.z < b.z;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    }
};

// MaterialFn(vx,vy,vz): the material at that world-space voxel (MAT_AIR for
// empty/passable). Unlike connectivity.h's boolean SolidFn, pathfinding
// needs the actual MaterialId to price mining by hardness and to hard-block
// MAT_BEDROCK — see the header comment's action model.
using MaterialFn = std::function<MaterialId(int64_t vx, int64_t vy, int64_t vz)>;

enum class Action : uint8_t {
    Walk = 0,
    StepUp = 1,
    StepDown = 2,
    Climb = 3,
    Fall = 4,
    Mine = 5,
    Bridge = 6,
    Jump = 7,
};

inline const char* actionName(Action a) {
    switch (a) {
        case Action::Walk: return "Walk";
        case Action::StepUp: return "StepUp";
        case Action::StepDown: return "StepDown";
        case Action::Climb: return "Climb";
        case Action::Fall: return "Fall";
        case Action::Mine: return "Mine";
        case Action::Bridge: return "Bridge";
        case Action::Jump: return "Jump";
    }
    return "?";
}

// The single cost function's tunable weights (header comment "Action
// model"). All fields should be >= 0 (A*/Dijkstra requires non-negative
// edge costs); mineCostByMaterial entries may be negative as an explicit
// "this material is impassable" sentinel (MAT_BEDROCK is impassable
// unconditionally regardless of this table — see header comment).
// mineCostByMaterial defaults to all-zero (free to mine) — a placeholder,
// not a usable default; callers must set real hardness x dig-time costs.
struct PathCostConfig {
    int32_t walkCost = 10;
    int32_t stepUpCost = 14;
    int32_t fallCostPerVoxel = 5;
    int32_t mineCostByMaterial[kMaterialCount] = {};
    int32_t bridgeCost = 20;
    int32_t jumpGapCost = 15;
};

// Bounds the search: only cells within the inclusive [minCorner, maxCorner]
// box are ever expanded, and at most maxExpansions cells are ever settled —
// see header comment "Search / windowing".
struct SearchWindow {
    PathCoord minCorner;
    PathCoord maxCorner;
    int32_t maxExpansions = 100000;
};

// One step of a path: the agent ends up AT `cell`. `affectedCell` is the
// voxel the action mutates the world at (only Mine — dug out — and Bridge —
// scaffold placed — actually imply an edit-log write; every other action's
// affectedCell equals cell and implies no edit, informational only).
struct PathStep {
    PathCoord cell;
    Action action = Action::Walk;
    PathCoord affectedCell;
    int32_t stepCost = 0;
};

struct PathResult {
    PathCoord start;
    PathCoord goal;
    std::vector<PathStep> steps; // ordered start -> reached (see `reached`)
    int64_t totalCost = 0;
    bool complete = false; // true iff `reached` == goal, found optimally
    bool capped = false;   // true iff the search stopped before reaching
                            // goal (window exhausted or maxExpansions hit);
                            // always == !complete (kept as its own field per
                            // the plan's "capped/complete flag" wording).
    PathCoord reached;     // final cell of `steps` (== start if steps empty)
    int32_t expansionsUsed = 0;

    // Deterministic digest over the full result (header comment
    // "Determinism") — regression/golden-test primitive, mirroring
    // ConnectivityResult::digest / WaterCA::digest (core.h's Digest,
    // FNV-1a 64).
    void digest(Digest& d) const {
        d.i64(start.x); d.i64(start.y); d.i64(start.z);
        d.i64(goal.x); d.i64(goal.y); d.i64(goal.z);
        d.i64(totalCost);
        d.u8(complete ? 1 : 0);
        d.u8(capped ? 1 : 0);
        d.i64(reached.x); d.i64(reached.y); d.i64(reached.z);
        d.u32(static_cast<uint32_t>(steps.size()));
        for (const PathStep& s : steps) {
            d.u8(static_cast<uint8_t>(s.action));
            d.i64(s.cell.x); d.i64(s.cell.y); d.i64(s.cell.z);
            d.i64(s.affectedCell.x); d.i64(s.affectedCell.y); d.i64(s.affectedCell.z);
            d.i64(s.stepCost);
        }
    }
};

// Fixed neighbor offsets — see header comment "Neighbor model". Order is
// not load-bearing for the optimal path found (only the priority queue's
// PathCoordLess tie-break is), kept fixed for readability/consistency with
// the rest of voxel-core's fixed-order conventions.
inline constexpr std::array<std::array<int64_t, 3>, 18> kNeighborOffsets = {{
    // 6-connected core: flat horizontal +-x/+-y, then vertical +-z.
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    // Step up (horizontal + one voxel up): +x,-x,+y,-y
    {1, 0, 1}, {-1, 0, 1}, {0, 1, 1}, {0, -1, 1},
    // Step down (horizontal + one voxel down): +x,-x,+y,-y
    {1, 0, -1}, {-1, 0, -1}, {0, 1, -1}, {0, -1, -1},
    // Jump (2-voxel horizontal leap over a 1-wide gap): +x,-x,+y,-y
    {2, 0, 0}, {-2, 0, 0}, {0, 2, 0}, {0, -2, 0},
}};

namespace detail {

inline size_t localIndex(int64_t lx, int64_t ly, int64_t lz, int64_t dx, int64_t dy) {
    return static_cast<size_t>(lx) +
           static_cast<size_t>(dx) *
               (static_cast<size_t>(ly) + static_cast<size_t>(dy) * static_cast<size_t>(lz));
}

inline bool inBox(const PathCoord& c, const PathCoord& lo, const PathCoord& hi) {
    return c.x >= lo.x && c.x <= hi.x && c.y >= lo.y && c.y <= hi.y && c.z >= lo.z && c.z <= hi.z;
}

inline int64_t manhattan(const PathCoord& a, const PathCoord& b) {
    const int64_t dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const int64_t dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    const int64_t dz = a.z > b.z ? a.z - b.z : b.z - a.z;
    return dx + dy + dz;
}

struct MoveClassification {
    bool valid = false;
    Action action = Action::Walk;
    int32_t cost = 0;
    PathCoord to;
    PathCoord affected;
};

// Classifies the single candidate move `from -> from+(dx,dy,dz)` per the
// header comment's action model. Pure function of its arguments — the same
// inputs always produce the same classification, which is what makes
// pathStillValid a correct recheck (header comment "Incremental
// invalidation").
inline MoveClassification classifyMove(const MaterialFn& solidFn, const PathCostConfig& config,
                                        PathCoord from, int64_t dx, int64_t dy, int64_t dz) {
    MoveClassification mv;
    const PathCoord to{from.x + dx, from.y + dy, from.z + dz};
    mv.to = to;
    mv.affected = to;

    const int64_t absdx = dx < 0 ? -dx : dx;
    const int64_t absdy = dy < 0 ? -dy : dy;
    const int64_t absdz = dz < 0 ? -dz : dz;

    const bool isJump = dz == 0 && ((absdx == 2 && dy == 0) || (absdy == 2 && dx == 0));
    const bool isVertical = dx == 0 && dy == 0 && absdz == 1;
    const bool isStepOrFlat =
        !isJump && absdz <= 1 && ((absdx == 1 && dy == 0) || (absdy == 1 && dx == 0));

    if (isJump) {
        const PathCoord mid{from.x + dx / 2, from.y + dy / 2, from.z};
        if (solidFn(mid.x, mid.y, mid.z) != MAT_AIR) return mv; // gap breached mid-air, no leap
        // A jump is only meaningful over an actual GAP: the midpoint must
        // have no support of its own (nothing to walk on), otherwise
        // leaping over ordinary walkable ground would let Jump silently
        // undercut Walk/StepUp any time jumpGapCost < 2x their cost, which
        // is not what "jump a gap" means.
        if (solidFn(mid.x, mid.y, mid.z - 1) != MAT_AIR) return mv; // midpoint is normal ground
        if (solidFn(to.x, to.y, to.z) != MAT_AIR) return mv;        // can't jump into solid
        if (solidFn(to.x, to.y, to.z - 1) == MAT_AIR) return mv;    // must land supported
        mv.valid = true;
        mv.action = Action::Jump;
        mv.cost = config.jumpGapCost;
        return mv;
    }

    if (!isVertical && !isStepOrFlat) return mv; // not one of the 18 offsets; defensive no-op

    const MaterialId destMat = solidFn(to.x, to.y, to.z);
    if (destMat != MAT_AIR) {
        if (destMat == MAT_BEDROCK) return mv; // unconditionally impassable
        const int32_t mineCost = config.mineCostByMaterial[destMat];
        if (mineCost < 0) return mv; // caller-marked impassable
        mv.valid = true;
        mv.action = Action::Mine;
        mv.cost = mineCost;
        return mv;
    }

    if (isVertical) {
        mv.valid = true;
        mv.action = dz > 0 ? Action::Climb : Action::Fall;
        mv.cost = dz > 0 ? config.stepUpCost : config.fallCostPerVoxel;
        return mv;
    }

    // Flat or step move into air: supported iff the cell directly below the
    // destination is solid.
    const bool supported = solidFn(to.x, to.y, to.z - 1) != MAT_AIR;
    if (dz == 0) {
        if (supported) {
            mv.valid = true;
            mv.action = Action::Walk;
            mv.cost = config.walkCost;
        } else {
            mv.valid = true;
            mv.action = Action::Bridge;
            mv.cost = config.bridgeCost;
            mv.affected = PathCoord{to.x, to.y, to.z - 1};
        }
        return mv;
    }
    if (dz > 0) {
        if (supported) {
            mv.valid = true;
            mv.action = Action::StepUp;
            mv.cost = config.stepUpCost;
        } else {
            mv.valid = true;
            mv.action = Action::Bridge;
            mv.cost = config.bridgeCost;
            mv.affected = PathCoord{to.x, to.y, to.z - 1};
        }
        return mv;
    }
    // dz < 0: step down only valid when supported; unsupported step-down is
    // not a v0 action (see header comment) — the search finds it as a
    // separate Fall instead.
    if (supported) {
        mv.valid = true;
        mv.action = Action::StepDown;
        mv.cost = config.walkCost;
    }
    return mv;
}

} // namespace detail

// Priority queue entry + comparator: min-cost first, PathCoordLess as the
// ONLY tie-break (header comment "Determinism").
struct PathQueueEntry {
    int64_t g;
    PathCoord coord;
};

struct PathQueueCmp {
    bool operator()(const PathQueueEntry& a, const PathQueueEntry& b) const {
        if (a.g != b.g) return a.g > b.g;
        return PathCoordLess{}(b.coord, a.coord);
    }
};

// Windowed dig-aware A* (Dijkstra, zero heuristic — see header comment
// "Search / windowing"). Returns the optimal path if the goal is reached
// within the window/expansion budget; otherwise a best-effort path to the
// settled node closest (Manhattan distance) to the goal, with capped=true.
inline PathResult findPath(const MaterialFn& solidFn, PathCoord start, PathCoord goal,
                            const PathCostConfig& config, const SearchWindow& window) {
    PathResult result;
    result.start = start;
    result.goal = goal;
    result.reached = start;

    if (window.maxCorner.x < window.minCorner.x || window.maxCorner.y < window.minCorner.y ||
        window.maxCorner.z < window.minCorner.z || !detail::inBox(start, window.minCorner, window.maxCorner)) {
        result.capped = true;
        return result;
    }

    const int64_t dx = window.maxCorner.x - window.minCorner.x + 1;
    const int64_t dy = window.maxCorner.y - window.minCorner.y + 1;
    const int64_t dz = window.maxCorner.z - window.minCorner.z + 1;
    const size_t volume = static_cast<size_t>(dx) * static_cast<size_t>(dy) * static_cast<size_t>(dz);

    auto localIdx = [&](const PathCoord& c) {
        return detail::localIndex(c.x - window.minCorner.x, c.y - window.minCorner.y,
                                   c.z - window.minCorner.z, dx, dy);
    };

    std::vector<uint8_t> settled(volume, 0);
    std::vector<uint8_t> hasParent(volume, 0);
    std::vector<int64_t> bestG(volume, INT64_MAX);
    std::vector<PathCoord> parentCoord(volume);
    std::vector<Action> parentAction(volume);
    std::vector<PathCoord> parentAffected(volume);
    std::vector<int32_t> parentStepCost(volume, 0);

    std::priority_queue<PathQueueEntry, std::vector<PathQueueEntry>, PathQueueCmp> open;
    const size_t startIdx = localIdx(start);
    bestG[startIdx] = 0;
    open.push({0, start});

    PathCoord bestNode = start;
    int64_t bestNodeDist = detail::manhattan(start, goal);
    int64_t bestNodeG = 0;

    auto reconstruct = [&](const PathCoord& target) {
        std::vector<PathStep> rev;
        PathCoord cur = target;
        size_t curIdx = localIdx(cur);
        while (hasParent[curIdx]) {
            PathStep s;
            s.cell = cur;
            s.action = parentAction[curIdx];
            s.affectedCell = parentAffected[curIdx];
            s.stepCost = parentStepCost[curIdx];
            rev.push_back(s);
            cur = parentCoord[curIdx];
            curIdx = localIdx(cur);
        }
        result.steps.assign(rev.rbegin(), rev.rend());
        result.reached = target;
        result.totalCost = bestG[localIdx(target)];
    };

    while (!open.empty()) {
        if (result.expansionsUsed >= window.maxExpansions) break;

        const PathQueueEntry e = open.top();
        open.pop();
        const size_t idx = localIdx(e.coord);
        if (settled[idx]) continue;
        if (e.g > bestG[idx]) continue; // stale entry

        settled[idx] = 1;
        ++result.expansionsUsed;

        const int64_t dist = detail::manhattan(e.coord, goal);
        if (dist < bestNodeDist ||
            (dist == bestNodeDist &&
             (e.g < bestNodeG || (e.g == bestNodeG && PathCoordLess{}(e.coord, bestNode))))) {
            bestNode = e.coord;
            bestNodeDist = dist;
            bestNodeG = e.g;
        }

        if (e.coord == goal) {
            reconstruct(e.coord);
            result.complete = true;
            result.capped = false;
            return result;
        }

        for (const auto& off : kNeighborOffsets) {
            const detail::MoveClassification mv =
                detail::classifyMove(solidFn, config, e.coord, off[0], off[1], off[2]);
            if (!mv.valid) continue;
            if (!detail::inBox(mv.to, window.minCorner, window.maxCorner)) continue;

            const size_t nIdx = localIdx(mv.to);
            const int64_t newG = e.g + mv.cost;
            if (newG < bestG[nIdx]) {
                bestG[nIdx] = newG;
                hasParent[nIdx] = 1;
                parentCoord[nIdx] = e.coord;
                parentAction[nIdx] = mv.action;
                parentAffected[nIdx] = mv.affected;
                parentStepCost[nIdx] = mv.cost;
                open.push({newG, mv.to});
            }
        }
    }

    // Goal not reached: exhausted the window's reachable region or hit
    // maxExpansions. Best-effort result toward the goal.
    reconstruct(bestNode);
    result.complete = (bestNode == goal);
    result.capped = !result.complete;
    return result;
}

// Incremental-invalidation primitive (header comment "Incremental
// invalidation"): re-classifies every step of `path` against `solidFn`/
// `config` and returns false the instant any step's action no longer
// matches what was recorded (destination changed from air to solid or vice
// versa, support disappeared, a material's mine-cost sign flipped, ...).
inline bool pathStillValid(const PathResult& path, const MaterialFn& solidFn,
                            const PathCostConfig& config) {
    PathCoord cur = path.start;
    for (const PathStep& s : path.steps) {
        const int64_t dx = s.cell.x - cur.x;
        const int64_t dy = s.cell.y - cur.y;
        const int64_t dz = s.cell.z - cur.z;
        const detail::MoveClassification mv = detail::classifyMove(solidFn, config, cur, dx, dy, dz);
        if (!mv.valid || mv.action != s.action) return false;
        cur = s.cell;
    }
    return true;
}

} // namespace vxc
