#pragma once
// Water pressure cellular automaton (plan §3.7 Layer B — THE AUTHORITY):
// engine-free CPU reference. Integer, bit-deterministic, volume-conserving.
// The GPU port (later, like the amplifier) MUST match this bit-exactly; this
// file IS the reference behavior, not an approximation of some "real" fluid
// sim. Any change to the tick rules below is world-breaking exactly like
// worldgen (docs/determinism.md): bump kWaterCAVersion and regenerate goldens
// when (and only when) a change is deliberate.
//
// Water never occupies solid cells; solidity comes from a caller-supplied
// MaterialId(vx,vy,vz) query (terrain, from World<B>::materialAt or
// equivalent) — this header has no terrain dependency, doctrine-clean.
//
// -----------------------------------------------------------------------
// Storage
// -----------------------------------------------------------------------
// WaterBrick8: a dense 8^3 array of per-voxel fill fraction (0 = empty,
// 255 = full), NOT a Brick<8> — water fill is a continuous quantity per
// cell, not a material palette index, so the palette/occupancy-bitmask
// design doesn't apply. "Homogeneous-empty collapse" here means: an
// all-zero brick is never stored at all (WaterMap erases a brick the
// instant its nonEmptyCount reaches zero) rather than Brick<B>'s in-place
// homogeneous representation — the natural null state for water is
// "absent from the map", since unlike terrain there is no other
// homogeneous fill value worth collapsing to.
//
// WaterMap: hashed map of WaterBrick8 keyed by the same BrickKey used for
// terrain bricks (brick.h) — brick coordinates are voxel coordinates
// floorDiv 8, independent of terrain's own brick edge size B.
//
// -----------------------------------------------------------------------
// Tick rules v1 (two-phase read-flows/apply — THE determinism contract)
// -----------------------------------------------------------------------
// v0 (kWaterCAVersion==1) was a single in-place sequential Gauss-Seidel
// sweep: correct and simple, but O(active cells) with a terrible constant
// factor (a hashmap lookup — BrickKey compute + find — for every cell AND
// every one of its up-to-4 neighbor reads/writes) and fundamentally
// unparallelizable, since each cell's result depended on cells already
// mutated earlier in the same pass. v1 (kWaterCAVersion==2) replaces it
// with a two-phase (read-flows, then apply) scheme that is DETERMINISTIC BY
// CONSTRUCTION and, unlike v0, does not depend on any particular iteration
// order over the active set — the property a future parallel/GPU port
// needs (see waterca_twophase_order_independent_and_deterministic in
// tests/test_waterca.cpp for the property test, and
// waterca_lateral_contention_capped_conserved_fixed_order for the
// conservation-under-contention proof). step() over the same scenario now
// produces different per-tick values and a different digest than v0 did —
// deliberate and expected (this is exactly the kind of change
// docs/determinism.md calls world-breaking); the settled *shape* a scenario
// converges to is the same physical fixed-point family, just reached via a
// different (now parallel-safe) per-tick path. Every stored WaterCA-derived
// save/golden must be re-pinned against v1; there is no v0 compatibility
// path.
//
// One step() call runs 8 sequential COLORED ROUNDS (round 0..7, always in
// that fixed order — itself part of the determinism contract, not a
// per-tick choice), each a complete READ -> GATHER -> FINALIZE pass:
//
//   COLOR: every cell has a fixed color = (x&1) | (y&1)<<1 | (z&1)<<2 (one
//   bit per axis parity, 8 possible values). Round c only ever computes
//   OUTGOING flow for cells whose color == c; every other active cell
//   contributes nothing that round (not filtered out of "active", just a
//   source of zero flow for that specific round). This 8-way split is
//   required for correctness, not just an optional speed trick — see
//   colorOf's comment in waterca.cpp for what was tried and rejected first
//   (a plain 2-color x^y^z-parity split fixes a naive single-pass Jacobi
//   update's checkerboard instability but NOT a small closed flow loop,
//   which can trade flow in a perfect non-progressing cycle forever under
//   only 2 colors; 8 colors — one per (x,y,z) parity combination — ensure
//   no two cells of any such small loop ever move on the same stale
//   snapshot). Round 0 reads/writes against true tick-start data; rounds
//   1-7 read whatever the previous round(s) already committed to the real
//   WaterMap this same tick (this is what makes 8 rounds converge properly
//   instead of everything moving simultaneously off one snapshot).
//
//   READ: for every active cell of round c's color, with fill > 0, compute
//   up to 5 outgoing flows in a FIXED priority order that is also the ONLY
//   tie-break rule in this whole scheme:
//     0. GRAVITY: if the cell below (z-1) is not solid, desired = min(self,
//        255 - belowFill). "Supported" (resting on solid OR a below cell at
//        255) gates whether lateral runs at all this round — an unsupported
//        (still-falling) cell only ever does gravity, exactly like v0.
//     1..4. LATERAL, +x, -x, +y, -y (fixed order, matching v0's kDx/kDy):
//        skip a solid neighbor; skip if self <= neighbor (flow only
//        downhill); flow = (self - neighbor) / 2 (a diff of 1 legitimately
//        produces 0 — that IS the "flat within +/-1" fixed point, not a
//        stall to work around); capped so the neighbor alone never passes
//        255.
//   SOURCE-SIDE CAP ("a cell's total outflow is capped at its own fill"):
//   the 5 desired flows above are accumulated against a per-cell budget
//   starting at that cell's own current fill, spent in the SAME fixed
//   order (gravity first, then +x,-x,+y,-y) — whichever flows exhaust the
//   budget first win it in full; anything after the budget hits zero is
//   truncated to 0 right here, before any target is ever consulted.
//
//   GATHER: every cell that could possibly receive flow this round (every
//   active cell of ANY color, plus its 5 target-direction neighbors — the
//   "touched" set, computed once per step() call and reused by every
//   round) is visited exactly once. For that cell, its (at most 5) POSSIBLE
//   inbound contributions — from the cell above (gravity) and from its
//   +x/-x/+y/-y neighbors' own lateral-toward-here flows — are gathered in
//   the SAME fixed order (gravity, then +x/-x/+y/-y, now read as "which
//   neighbor's flow lands here") against a budget starting at
//   (255 - thisCell'sCurrentFill): this is "competing inflows to one
//   target are resolved in a fixed deterministic order so total is
//   conserved exactly" — whichever inbound edges come first in the fixed
//   order are admitted in full up to the remaining budget; once the budget
//   hits zero, every remaining candidate this round is admitted as 0,
//   regardless of what it "wanted" to send. The admitted (possibly
//   truncated) amount for each edge is written back into the SOURCE cell's
//   own bookkeeping (each edge is visited by exactly one target cell, so
//   there is no write race and no dependency on iteration order).
//   FINALIZE: once every touched cell has been gathered this round (so
//   every active cell's outgoing edges have all been decided by their
//   targets), each touched cell's new fill = currentFill + admittedInflow -
//   admittedOutflow, committed immediately to the real WaterMap (so the
//   NEXT round already sees it). Every admitted unit appears in exactly one
//   cell's inflow and exactly one cell's outflow, so summed over all cells
//   the net change is exactly zero every round — volume conservation is a
//   structural property of this scheme, not a checked-after-the-fact
//   invariant.
//
// "Changed" (the next tick's active set) is NOT derived from individual
// per-round writes: because round c+1 can (and often does) partially undo
// round c's own write on the SAME cell, comparing tick-START to tick-END
// state (a snapshot taken once, before round 0, compared once, after round
// 7) is what makes "changed" reflect NET tick-over-tick change rather than
// flagging a brick active forever over writes that cancel out within the
// tick. The next active set is `changed` UNION every one of `changed`'s 6
// face-neighbors (not just the 5 "outgoing target" directions `touched`
// uses) — a brick can be completely blocked for an entire tick (e.g. its
// gravity target still full) and therefore be a source/target of nothing,
// yet become unblockable the moment that blocking neighbor drains; without
// this 6-direction reactivation such a brick can never be reconsidered
// again once it loses one single-tick race.
//
// This whole per-tick pipeline is a pure function of (which bricks were
// active at entry, and the WaterMap's stored contents at entry) — NOT of
// any iteration order over the active set. stepWithOrder() exposes this
// directly (feed it the active bricks in any order — sorted, reversed,
// shuffled — and get back the identical resulting WaterMap contents and the
// identical next active set); step() is simply stepWithOrder() called with
// the real active_ set's contents.
//
// Phase C — HYDROSTATIC (stub, documented hook only): full column pressure
// (fills U-bends; breaching below water table = depth-scaled inrush jet) is
// W2-proper, not v1. hydrostaticPass() exists as a no-op so the
// read/apply -> hydrostatic pipeline shape is fixed now; implementing it
// later must preserve both the volume-conservation invariant and the
// active-set-order independence the same way Phase READ/APPLY do.
//
// -----------------------------------------------------------------------
// Activity / settling
// -----------------------------------------------------------------------
// A brick becomes active when addWater() touches it, or when a step()
// pass changes any of its cells (as source OR destination of a move —
// this is how "neighbors reactivate on change" is satisfied: the moment a
// neighboring brick's boundary cell changes, that brick's key is itself
// marked changed and is therefore active next tick). The new active set
// for the next tick is EXACTLY the set of bricks that changed this tick;
// a brick that produced no change this tick "settles out" (drops out of
// the active set) even if it was active coming in. step() over a fully
// settled state (empty active set) touches zero bricks.
//
// -----------------------------------------------------------------------
// Conservation
// -----------------------------------------------------------------------
// totalVolume() is a running ledger (uint64_t), updated incrementally by
// every fill-changing operation — it is NOT recomputed by walking bricks
// on every call. step() must never change it (no fill is created or
// destroyed by gravity/lateral, only moved); only addWater() may increase
// it. recomputeVolume() independently re-sums every stored brick's fill
// and exists for callers (tests, asserts) to cross-check the ledger
// against actual stored state.

#include <cstdint>
#include <functional>
#include <set>
#include <unordered_map>
#include <vector>

#include "voxelcore/brick.h"
#include "voxelcore/core.h"

namespace vxc {

// Bumped on any deliberate change to the tick rules above (gravity,
// lateral, future hydrostatic). Invalidates saved water-sim state and
// golden digests, exactly like kWorldGenVersion.
//
// v1 (==1): sequential in-place Gauss-Seidel sweep (retired).
// v2 (==2): two-phase read-flows/apply scheme, active-set-order
// independent — see "Tick rules v1" header comment above (the CONTRACT is
// versioned "v1" in prose since it's the first two-phase contract; the
// bumped kWaterCAVersion constant is the actual invalidation signal).
inline constexpr uint32_t kWaterCAVersion = 2;

// Dense 8^3 fill-fraction brick. 0 = empty, 255 = full. Always brick edge
// 8 (not templated like Brick<B> — water ticks at a fixed cell size).
class WaterBrick8 {
public:
    static constexpr int kEdge = 8;
    static constexpr int kCells = kEdge * kEdge * kEdge;

    static constexpr int cellIndex(int x, int y, int z) { return x + kEdge * (y + kEdge * z); }

    uint8_t get(int x, int y, int z) const { return fill_[cellIndex(x, y, z)]; }

    void set(int x, int y, int z, uint8_t v) {
        uint8_t& cell = fill_[cellIndex(x, y, z)];
        if (cell == 0 && v != 0) ++nonEmptyCount_;
        else if (cell != 0 && v == 0) --nonEmptyCount_;
        cell = v;
    }

    bool empty() const { return nonEmptyCount_ == 0; }
    uint32_t nonEmptyCount() const { return nonEmptyCount_; }

    // Sum of this brick's fill (max 512*255 = 130,560; fits comfortably).
    uint64_t volume() const {
        uint64_t sum = 0;
        for (uint8_t f : fill_) sum += f;
        return sum;
    }

    // Deterministic digest contribution (cell order, matching Brick<B>::digest
    // and the tick rules' own fixed cell order).
    void digest(Digest& d) const {
        for (uint8_t f : fill_) d.u8(f);
    }

private:
    uint8_t fill_[kCells] = {}; // zero-initialized: empty
    uint32_t nonEmptyCount_ = 0;
};

// Hashed map of WaterBrick8 keyed by BrickKey (brick.h) at brick edge 8.
// Mirrors ChunkMap<B>'s shape (chunkmap.h) but is not a template, since
// WaterBrick8 is fixed-edge. An all-empty brick is never left stored (see
// WaterCA's setFillAccounted): homogeneous-empty collapse == absence.
class WaterMap {
public:
    WaterBrick8* find(const BrickKey& k) {
        auto it = bricks_.find(k);
        return it == bricks_.end() ? nullptr : &it->second;
    }
    const WaterBrick8* find(const BrickKey& k) const {
        auto it = bricks_.find(k);
        return it == bricks_.end() ? nullptr : &it->second;
    }
    WaterBrick8& getOrCreate(const BrickKey& k) { return bricks_[k]; }
    void erase(const BrickKey& k) { bricks_.erase(k); }

    size_t size() const { return bricks_.size(); }
    auto begin() { return bricks_.begin(); }
    auto end() { return bricks_.end(); }
    auto begin() const { return bricks_.begin(); }
    auto end() const { return bricks_.end(); }

private:
    std::unordered_map<BrickKey, WaterBrick8, BrickKeyHash> bricks_;
};

inline BrickKey waterKeyForVoxel(int64_t vx, int64_t vy, int64_t vz) {
    return BrickKey{static_cast<int32_t>(floorDiv(vx, WaterBrick8::kEdge)),
                    static_cast<int32_t>(floorDiv(vy, WaterBrick8::kEdge)),
                    static_cast<int32_t>(floorDiv(vz, WaterBrick8::kEdge))};
}

class WaterCA {
public:
    // Terrain solidity query: MAT_AIR means "not solid" (water may occupy
    // it), anything else means solid (water never occupies it). Supplied by
    // the caller (e.g. World<B>::materialAt) — this header stays
    // terrain-free by doctrine.
    using SolidFn = std::function<MaterialId(int64_t vx, int64_t vy, int64_t vz)>;

    explicit WaterCA(SolidFn solid) : solid_(std::move(solid)) {}

    // Adds `amount` fill units starting at (vx,vy,vz), stacking straight up
    // (vz, vz+1, ...) as each cell fills to capacity (255); stops early if
    // it runs into a solid cell (nowhere left to place water in that
    // column). Returns the amount actually placed, which is < amount only
    // in that early-stop case — the conservation ledger (totalVolume())
    // only ever accounts for what was actually placed, so callers that need
    // an exact-conservation invariant should track this return value, not
    // the requested amount. Marks every touched brick active.
    uint32_t addWater(int64_t vx, int64_t vy, int64_t vz, uint32_t amount);

    uint8_t fillAt(int64_t vx, int64_t vy, int64_t vz) const { return getFill(vx, vy, vz); }

    // Running conservation ledger (see header comment). O(1).
    uint64_t totalVolume() const { return totalVolume_; }

    // Independent re-sum of every stored brick's fill. O(bricks*512); for
    // cross-checking the ledger, not for hot-path use.
    uint64_t recomputeVolume() const;

    // One tick: two-phase read/apply over the active set snapshot taken at
    // entry (see header comment "Tick rules v1"), then the (currently
    // no-op) hydrostatic hook. Equivalent to
    // stepWithOrder(activeSetSnapshot()).
    void step();

    // Current active set as a plain vector (BrickKeyLess order — but see
    // stepWithOrder: the ORDER of this vector is never load-bearing for the
    // tick's result, only its CONTENTS are).
    std::vector<BrickKey> activeSetSnapshot() const {
        return std::vector<BrickKey>(active_.begin(), active_.end());
    }

    // Runs one two-phase tick using `order` as the active-brick set instead
    // of the real active_ set's own snapshot (duplicates are ignored; any
    // permutation of the same key set is accepted and — this is the whole
    // point — produces byte-identical resulting WaterMap contents and next
    // active set no matter what order `order` lists them in). step() is
    // exactly stepWithOrder(activeSetSnapshot()). Exists so tests (and,
    // later, a GPU port's own dispatch-order-agnostic scheduling) can prove
    // the order-independence property directly instead of only inferring it
    // from step()'s always-sorted internal traversal.
    void stepWithOrder(std::vector<BrickKey> order);

    // Number of bricks the most recent step()/stepWithOrder() call was given
    // as its active-set snapshot (post-dedup) — 0 exactly when the sim was
    // fully settled.
    size_t steppedBrickCount() const { return lastSteppedBrickCount_; }
    size_t activeBrickCount() const { return active_.size(); }
    size_t storedBrickCount() const { return water_.size(); }

    // Deterministic digest over every stored brick (not just active ones),
    // in sorted BrickKey order — the determinism/regression-test primitive,
    // mirroring Brick<B>::digest / World::editedDigest.
    void digest(Digest& d) const;

private:
    bool isSolid(int64_t vx, int64_t vy, int64_t vz) const { return solid_(vx, vy, vz) != MAT_AIR; }
    uint8_t getFill(int64_t vx, int64_t vy, int64_t vz) const;

    // Writes newFill at (vx,vy,vz), updating the conservation ledger by the
    // delta and collapsing the owning brick out of the map if it becomes
    // empty. If `changed` is non-null and the write is an actual change,
    // records the owning brick's key into it (the step()-local "what
    // changed this tick" set that becomes the next active set).
    void setFillAccounted(int64_t vx, int64_t vy, int64_t vz, uint8_t newFill,
                          std::set<BrickKey, BrickKeyLess>* changed);

    void activate(const BrickKey& k) { active_.insert(k); }

    // v0 stub — see header comment "Phase C". Intentionally a no-op.
    void hydrostaticPass(const std::vector<BrickKey>& order, std::set<BrickKey, BrickKeyLess>& changed);

    SolidFn solid_;
    WaterMap water_;
    std::set<BrickKey, BrickKeyLess> active_;
    uint64_t totalVolume_ = 0;
    size_t lastSteppedBrickCount_ = 0;
};

} // namespace vxc
