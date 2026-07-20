#pragma once
// Water pressure cellular automaton v0 (plan §3.7 Layer B — THE AUTHORITY):
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
// Tick rules v0 (fixed sweep order — THE determinism contract)
// -----------------------------------------------------------------------
// step() processes exactly the bricks in the active set as of the START of
// the call (a snapshot), in two full passes over that same snapshot, each
// pass visiting cells in a single FIXED global order:
//   - bricks: ascending BrickKeyLess (z, then y, then x)
//   - cells within a brick: ascending cellIndex = x + 8*(y + 8*z), i.e.
//     z ascending (bottom of the brick to top), y then x as inner loops
// Every read/write is sequential and in-place — a cell processed later in
// the pass sees the effects of every cell processed earlier. This is
// deliberately NOT a read-flows-then-apply scheme; v0 IS the reference
// behavior. A future parallel/GPU port needs a two-phase (read flows into
// a scratch buffer, then apply) scheme to be parallelizable, and MUST be
// verified bit-exact against this sequential reference the same way the
// amplifier's GPU port is verified against its CPU reference.
//
// Phase A — GRAVITY: for each active cell with fill > 0, if the cell
// directly below (z-1) is not solid and has fill < 255, move
// min(selfFill, 255 - belowFill) units down into it. A cell resting on
// solid ground or a full cell below does nothing this phase (that's what
// "resting on support" means for phase B). Falls are (by construction of
// the single-pass sweep) at most one level per tick per cell — multi-level
// drops take multiple step() calls, which is the intended v0 behavior.
//
// Phase B — LATERAL: for each active cell with fill > 0 that is now
// "resting on support" (the cell below is solid OR has fill == 255) after
// phase A, equalize with its 4 horizontal neighbors in FIXED order
// (+x, -x, +y, -y). For each neighbor in turn (re-reading this cell's
// current fill before each, since an earlier neighbor in the same phase
// may have already drained some of it):
//   - skip a solid neighbor entirely (water never crosses into solid)
//   - skip if self <= neighbor (flow only ever moves from higher to lower)
//   - flow = (self - neighbor) / 4, but if self > neighbor + 1 and that
//     division floors to 0, flow = 1 instead (guarantees forward progress
//     on a difference of 2 or 3 units, which integer /4 alone would stall
//     on forever) — the natural fixed point is neighbors within +/-1 of
//     each other, matching the pooling test's flatness check
//   - capped so self never goes negative and neighbor never exceeds 255
// Phase B only ever touches cells belonging to bricks in the START-of-tick
// active snapshot; a brick that is newly touched (by gravity or lateral
// overflow into brick-adjacent territory) becomes active for the NEXT
// tick instead of getting phase B treatment in this one — activity
// propagates (at least) one brick per tick, which is deterministic and
// keeps the per-tick cost bounded by the snapshot size.
//
// Phase C — HYDROSTATIC (v0 STUB, documented hook only): full column
// pressure (fills U-bends; breaching below water table = depth-scaled
// inrush jet) is W2-proper, not v0. hydrostaticPass() exists as a no-op so
// the gravity -> lateral -> hydrostatic pipeline shape is fixed now;
// implementing it later must preserve the volume-conservation invariant
// and the fixed global sweep order the same way phases A/B do.
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
inline constexpr uint32_t kWaterCAVersion = 1;

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

    // One tick: gravity, then lateral equalization, then the (currently
    // no-op) hydrostatic hook, over the active set snapshot taken at entry.
    // The new active set is exactly the bricks that changed this tick.
    void step();

    // Number of bricks the most recent step() call iterated (the active-set
    // snapshot size at entry) — 0 exactly when the sim was fully settled.
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

    void gravityPass(const std::vector<BrickKey>& order, std::set<BrickKey, BrickKeyLess>& changed);
    void lateralPass(const std::vector<BrickKey>& order, std::set<BrickKey, BrickKeyLess>& changed);
    // v0 stub — see header comment "Phase C". Intentionally a no-op.
    void hydrostaticPass(const std::vector<BrickKey>& order, std::set<BrickKey, BrickKeyLess>& changed);

    SolidFn solid_;
    WaterMap water_;
    std::set<BrickKey, BrickKeyLess> active_;
    uint64_t totalVolume_ = 0;
    size_t lastSteppedBrickCount_ = 0;
};

} // namespace vxc
