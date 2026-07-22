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
// Phase C — HYDROSTATIC (kWaterCAVersion==3): connected-volume level
// equalization, run once per tick after all 8 colored READ/APPLY rounds have
// committed. This is what makes water fill a U-bend and communicating
// vessels equalize, instead of only ever settling to each column's own LOCAL
// gravity+lateral fixed point (which a U-bend's far arm can never reach —
// gravity never flows up, and lateral only trades flow between same-z
// neighbors, so nothing in Phase READ/APPLY ever pushes water up the far
// arm no matter how much sits in the near one).
//
// ALGORITHM (per stepWithOrder() call, i.e. once per tick, not once per
// round):
//   1. CONNECTED-COMPONENT DISCOVERY: a bounded flood fill over voxel
//      6-neighbors, SEEDED ONLY FROM WATER CELLS (fill > 0) found by
//      scanning every brick in `touched` (the same active-plus-
//      target-neighbors set Phase READ/APPLY already computed this tick —
//      see stepWithOrder); a bare empty cell is never a seed (see the
//      empty-cell rule below for why letting one be a seed is actively
//      dangerous, not just useless). From there, which neighbors the fill
//      may step onto differs by what the neighbor cell IS:
//        - a cell that currently holds water (fill > 0, an O(1) WaterMap
//          lookup): ALWAYS explorable, unconditionally, regardless of
//          whether its owning brick is in `touched`, from ANY current cell.
//          This is deliberate and load-bearing — without it, a settled (no
//          longer active) stretch of a body of water would silently
//          disappear from the volume accounting the instant it stopped
//          changing, making "fill a U-bend" impossible whenever the source
//          arm has already settled (the near-universal case: gravity/
//          lateral drain a pour to its local fixed point in a handful of
//          ticks, long before hydrostatic has had time to raise the far
//          arm).
//        - a cell that's currently empty (fill == 0): explorable ONLY if
//          ALL THREE hold: (a) the step is not -z — never explore
//          DOWNWARD into previously-untouched empty space (any water cell
//          this pass ever looks at already finished settling this tick's
//          gravity/lateral rounds; if it had empty space below it, it
//          wouldn't be resting there — a -z step off it only ever reaches
//          dry floor BESIDE the real body, not real headroom, and letting
//          it through is what let an earlier version of this pass leak
//          into fresh dry land beside an open, unwalled pool and never
//          settle); (b) the CURRENT cell (the one this step is FROM) is
//          either a FULL water cell (fill==255 — "this column is
//          saturated, there may be pressure to rise") or an already-
//          included empty cell (fill==0 — continuing a chain legitimately
//          started at a full column); and (c) the neighbor's owning brick
//          is in `touched` — the actual "bounded to the active region"
//          cap, limiting how far into new headroom one tick's pass can
//          reach. Condition (b) is the one that makes seeding from a bare
//          empty cell dangerous: without it (and without gating on
//          fullness at all), a mostly-empty brick sharing its 8-voxel
//          height with a barely-filled water cell would pull that whole
//          brick's dry headroom into the redistribution EVERY tick purely
//          because it's the same brick, diluting the level and
//          reactivating a permanently growing footprint with no genuine
//          pressure behind it — measured empirically: an earlier version
//          of this pass gated only on `touched`-membership (no fullness
//          check, no -z exclusion) and waterca_pooling_spreads_flat_
//          within_tolerance never settled.
//      A component that needs more headroom than this tick's `touched`
//      provides simply rises partway; the water moved into newly-nonzero
//      cells reactivates their owning bricks (ordinary "changed" tracking
//      below) which — via stepWithOrder's own existing 6-direction
//      reactivation, see "Activity / settling" below — pulls the NEXT
//      brick up in +z into next tick's active set, so the reachable
//      ceiling climbs over the following ticks until the far arm reaches
//      the true equalized level (water-side reach is unbounded per the
//      first bullet, so only the "new, previously-dry" side of a rise is
//      ever tick-limited — an already-full column of water is never
//      artificially capped).
//      A hard cell-count cap per component (kMaxHydrostaticComponentCells,
//      waterca.cpp) is the backstop against the unbounded-through-water half
//      of this rule making one enormous connected body (a whole persistent
//      lake/ocean, later milestones) expensive every tick something merely
//      touches its edge: a component that would exceed the cap is left
//      completely unmodified this tick (never partially/incorrectly
//      equalized off a truncated view) — safe, just deferred; see
//      docs/status.md's W2 hydrostatic note for the "active-only"
//      optimization (e.g. a persisted per-body union-find) this should get
//      before a real large persistent body exists.
//   2. LEVEL COMPUTATION: for each discovered component with at least one
//      water cell and at least 2 cells total (trivial size-1 "components"
//      are already their own fixed point — skip), sum its total current
//      fill (`totalVol`, exactly conserved — this is a REDISTRIBUTION of
//      existing volume among the component's own cells, never a source or
//      sink) and sort its cells by z ascending (ties broken by (x,y)
//      ascending — an arbitrary but FIXED, deterministic rule, exactly like
//      the lateral rule's own east/west-before-north/south tie-break).
//      Walking layers bottom-up, each layer's `n` cells get target fill 255
//      apiece (consuming n*255 of `totalVol`) as long as enough volume
//      remains to fill the WHOLE layer; the first layer that can't be fully
//      filled gets `totalVol / n` apiece plus one extra unit each to the
//      first `totalVol % n` cells in the fixed (x,y) tie-break order; every
//      layer above that gets 0. totalVol never exceeds the component's
//      total capacity (cells*255) since it's already the sum of THOSE SAME
//      cells' own current fill, each already <= 255 — the allocation always
//      has somewhere to put every unit, by construction, no separate
//      capacity check needed.
//   3. APPLY: each cell whose computed target differs from its current fill
//      is written via the same setFillAccounted() path (and the SAME
//      per-tick `changed` set) everything else in stepWithOrder uses —
//      ledger update, homogeneous-empty collapse, and activity tracking all
//      stay centralized there. A no-op write (target == current, the common
//      case once near equilibrium) costs nothing extra: setFillAccounted
//      already early-returns on equal old/new.
//
// CONSERVATION: structural, not checked-after-the-fact, for the same reason
// Phase READ/APPLY's is — every write is an absolute target computed from a
// partition of `totalVol` that sums back to exactly `totalVol` (integer
// division + explicit remainder distribution, no rounding loss), and
// distinct components never share a cell (each cell is visited at most once
// across the whole flood fill, via the shared `visited` set), so summing
// the deltas of every write across every component this tick nets to
// exactly zero.
//
// ORDER INDEPENDENCE: hydrostaticPass() takes `touched` (the SAME
// std::set<BrickKey> Phase READ/APPLY already built from `order`'s
// CONTENTS, never its permutation) and reads current WaterMap state — which
// is itself already order-independent by the time hydrostaticPass runs, by
// Phase READ/APPLY's own property. hydrostaticPass never looks at `order`
// itself, only `touched`'s contents and water_'s contents, both of which
// are already permutation-invariant; component discovery is an intrinsic
// property of the (touched-contents, water-contents) graph, and every
// tie-break inside it (z then (x,y)) is a fixed rule over voxel
// coordinates, never over iteration/discovery order. So hydrostaticPass is
// exactly as order-independent as Phase READ/APPLY, by the same argument.
//
// CONVERGENCE: for a single, isolated U-bend/communicating-vessels scenario
// (finite total volume, finite cross-sections), each tick's pass can only
// ever move water toward the fully-connected-component equilibrium level
// (never past it — the bottom-up allocation always produces the CORRECT
// equilibrium for whatever portion of the component is currently visible),
// and the visible portion is monotonically non-shrinking tick over tick
// (once a brick is reachable via the water-side unbounded rule it stays
// reachable, since water cells, once created, keep the owning brick
// eligible; touched-region growth from the existing 6-direction
// reactivation rule is also monotonic while anything is still changing).
// So the visible-equilibrium level is monotonically non-decreasing on the
// rising side and the process has a finite ceiling (the scenario's true
// full-container equilibrium) it cannot overshoot — it must reach and then
// hold that fixed point in finitely many ticks. Proved empirically in
// tests/test_waterca.cpp (waterca_hydrostatic_u_bend_equalizes_and_settles,
// waterca_hydrostatic_communicating_vessels_equalize) by running to
// settling and checking both arms/tanks land within +/-1 of each other.
//
// NOT MODELED in v0 (later milestones, see plan §3.7 Layer B/C split): flow
// MOMENTUM/velocity (a depth-scaled breach "inrush jet" needs Layer C's SWE
// patches for that; this pass only ever computes a static equilibrium
// level, instantly-ish over a few ticks, never a surge) and any
// active-region-crossing optimization for very large persistent bodies (the
// kMaxHydrostaticComponentCells cap above).
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

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>
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
// v3 (==3): Phase C hydrostatic pass is now real (connected-volume level
// equalization — see "Phase C — HYDROSTATIC" header comment below) instead
// of the v2 no-op stub. Deliberate world-breaking change per
// docs/determinism.md: step() now produces different per-tick values (and a
// different digest) than v2 did on any scenario where hydrostatic actually
// moves water (e.g. a U-bend) — the two-phase Phase READ/APPLY rounds
// themselves are UNCHANGED, only the new Phase C pass appended after them
// is new behavior. Every stored WaterCA-derived save/golden must be
// re-pinned against v3; there is no v2 compatibility path.
// v4 (==4): wakeRegion() exists and the engine calls it on every terrain
// edit (see "Terrain-edit reactivation" below). NO TICK RULE CHANGED — every
// pinned voxel-core golden below is byte-identical to v3, because no
// voxel-core scenario calls wakeRegion — but a live world's water now
// genuinely reacts to digs/places/carves/collapses instead of sitting frozen,
// so the same terrain-edit sequence replayed against a v3 recording no longer
// reproduces v3's (buggy, unchanging) water state. Per docs/determinism.md
// that divergence is what the version constant exists to signal.
inline constexpr uint32_t kWaterCAVersion = 4;

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

    // --- Single-cell accounted add/remove (W4 CA<->SWE coupling hook) ------
    //
    // WHY THESE EXIST AND WHY THEY ARE NOT addWater()/setReplicatedFill().
    // The SWE coupling (voxelcore/swe.h §5) needs to hand an exact integer
    // number of fill units to ONE named CA cell and take an exact integer
    // number back out of ONE named CA cell, and needs the owning brick woken
    // so the CA actually carries the water away afterwards. Neither existing
    // entry point does that: addWater() STACKS UPWARD as each cell fills,
    // which for a lake draining through a punctured bed would push water back
    // up into the very sheet it just left; and setReplicatedFill() is
    // documented client-mirror-only, overwrites rather than adds, and
    // deliberately does not wake anything, so water injected through it would
    // sit frozen.
    //
    // These are pure state edits at the same layer as addWater(): they use the
    // same setFillAccounted() ledger path and the same activate() wake, they
    // saturate rather than overflow (add is capped by 255-current and by
    // solidity; remove is capped by what is present), and they return the
    // amount ACTUALLY moved so a caller can keep an exact two-sided ledger.
    //
    // NO TICK RULE CHANGES and kWaterCAVersion is NOT bumped: nothing in
    // voxel-core calls these except the (default-OFF) coupler, no golden
    // scenario reaches them, and a world in which they are never called is
    // bit-for-bit identical to one in which they do not exist.
    uint32_t addWaterAt(int64_t vx, int64_t vy, int64_t vz, uint32_t amount);
    uint32_t removeWaterAt(int64_t vx, int64_t vy, int64_t vz, uint32_t amount);

    uint8_t fillAt(int64_t vx, int64_t vy, int64_t vz) const { return getFill(vx, vy, vz); }

    // Running conservation ledger (see header comment). O(1).
    uint64_t totalVolume() const { return totalVolume_; }

    // Independent re-sum of every stored brick's fill. O(bricks*512); for
    // cross-checking the ledger, not for hot-path use.
    uint64_t recomputeVolume() const;

    // One tick: two-phase read/apply over the active set snapshot taken at
    // entry (see header comment "Tick rules v1"), then the hydrostatic
    // connected-volume level-equalization pass (see header comment "Phase C
    // — HYDROSTATIC"). Equivalent to stepWithOrder(activeSetSnapshot()).
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

    // --- Read-only query accessors (W2 engine-integration groundwork) -----
    // Pure queries over existing state; none of these change tick behavior
    // or the determinism contract above, so they need no kWaterCAVersion
    // bump. Added for callers that need to know WHICH bricks/cells changed
    // (dirty-chunk re-mesh, replication diff encoding), not just counts
    // (steppedBrickCount/activeBrickCount already cover that).

    // The active set as of the end of the most recent step() call — per the
    // "Activity / settling" contract above, this IS exactly the set of
    // bricks that changed on that call (or the full initial active set if no
    // step() has run yet, e.g. right after addWater()).
    const std::set<BrickKey, BrickKeyLess>& activeBricks() const { return active_; }

    // Direct brick lookup (nullptr if absent/empty-collapsed) — for callers
    // that need a brick's full 512-cell content (meshing, replication
    // snapshotting) rather than one cell at a time via fillAt().
    const WaterBrick8* findBrick(const BrickKey& k) const { return water_.find(k); }

    // Every currently-stored (non-empty) brick, for callers that need to
    // enumerate the whole live water body (e.g. a full-state replication
    // snapshot, or rebuilding every render chunk after a reconnect).
    const WaterMap& bricks() const { return water_; }

    // Overwrites a single cell's fill with an authoritative value received
    // over the wire (client-side replication mirror only — NOT part of the
    // step()/addWater() simulation API). Keeps totalVolume()'s ledger
    // consistent via the same accounting setFillAccounted uses internally,
    // but — unlike addWater() — can move the ledger in either direction and
    // never stacks into neighboring cells: a replicated snapshot sets cells
    // to their authoritative values directly. Never called on the authority
    // instance's own simulated CA, only on a client's locally-held mirror.
    void setReplicatedFill(int64_t vx, int64_t vy, int64_t vz, uint8_t newFill) {
        setFillAccounted(vx, vy, vz, newFill, nullptr);
    }

    // --- Terrain-edit reactivation (wakeRegion) ---------------------------
    //
    // THE BUG THIS FIXES. Per "Activity / settling" above, a brick that
    // produces no net change in a tick drops out of the active set, and
    // step() over an empty active set is a no-op. That is correct and cheap
    // for a world whose terrain never changes — but the CA has no idea when
    // terrain DOES change. Before this API existed, the only way a settled
    // brick could ever tick again was addWater() (which injects volume) or
    // the caller happening to have some other water land next to it. So a
    // settled pond sat frozen forever no matter what was dug out from
    // underneath it: verified empirically (docs/status.md, W2) as an
    // unchanging water digest across an entire dig/place/carve/collapse
    // sequence. invalidateSolidRegion() only fixes the SOLIDITY MEMO's view
    // of the edit; it does not (and must not) schedule anything.
    //
    // WHAT THIS DOES. Re-inserts into the active set every brick that
    // CURRENTLY STORES WATER and intersects the edited voxel box grown by
    // kWakeHaloBricks brick in each direction on each axis. It writes no
    // fill whatsoever, so totalVolume() is unchanged by construction —
    // waking is purely a SCHEDULING act, never a source or sink. Returns the
    // number of bricks newly woken (diagnostics/tests only).
    //
    // THE NEIGHBORHOOD RULE, AND WHY. Waking only the bricks the edit
    // literally overlaps is not enough: the whole point is that water NEXT
    // TO a newly opened hole must now flow into it, and that water usually
    // lives in a different brick from the voxels the dig removed (dig the
    // floor out from under a pond and the removed voxels are all solid
    // terrain — there is no water in the edited bricks at all). One brick of
    // halo in each of x/y/z covers every face-, edge- and corner-adjacent
    // brick, which is the complete set of bricks whose cells can be within
    // one CA step's reach (gravity/lateral are 1-voxel moves) of any voxel
    // the edit changed. It does NOT need to be larger than that: once ANY
    // woken brick actually moves water, stepWithOrder's existing "changed
    // UNION changed's 6 face-neighbors" rule carries the activity outward on
    // its own, tick by tick, as far as the water actually travels. So the
    // halo only has to seed the reaction, not predict its extent — which is
    // also what keeps a big edit beside a big lake from re-activating the
    // whole lake up front (only the lake's edit-adjacent rim wakes; the rest
    // wakes only if and as water genuinely reaches it).
    //
    // ONLY WATER-BEARING BRICKS. An empty brick in the halo is skipped: it
    // has no fill to move, so activating it would contribute nothing to any
    // round while still costing a full 512-cell scan every tick until it
    // settled back out. Water flowing INTO a currently-empty brick is
    // already handled — the SOURCE brick is active, and stepWithOrder's
    // `touched` set already includes every active cell's target-direction
    // neighbors regardless of whether they hold water.
    //
    // DETERMINISM. The result is a pure function of (region, current
    // WaterMap contents): the active set is a std::set ordered by
    // BrickKeyLess, so insertion order is not observable, and the wake test
    // ("does this brick key currently store water?") is per-brick and
    // independent of any iteration or hash order. Enumerating the region's
    // bricks vs. enumerating the stored bricks (the size-based strategy
    // switch below) therefore yields the identical resulting active set —
    // proved in-suite by waterca_wake_region_order_and_strategy_independent.
    //
    // WHY THIS BUMPS kWaterCAVersion. No tick RULE changed, and no
    // voxel-core golden scenario calls this — but a live world's water now
    // evolves differently across an identical terrain-edit sequence than it
    // did before (that is the fix), so any recorded/replayed edit stream or
    // persisted water state from before this is no longer reproducible.
    // Per docs/determinism.md that is a version bump.
    static constexpr int kWakeHaloBricks = 1;
    size_t wakeRegion(int64_t minVx, int64_t minVy, int64_t minVz, int64_t maxVx, int64_t maxVy,
                      int64_t maxVz);

    // --- Cross-tick terrain-solidity cache (OPT-IN; OFF by default) --------
    //
    // WHAT IT IS. A pure MEMO of the caller-supplied `solid_` query, keyed by
    // voxel, persisting ACROSS ticks (unlike stepWithOrder's `cachedSolid`,
    // which is rebuilt every tick and therefore can never go stale). Memoizing
    // a pure function cannot change its answers, so with the cache enabled the
    // tick output is byte-for-byte identical to the uncached path FOR AS LONG
    // AS the memo agrees with `solid_` — which is exactly the caller's
    // obligation below. It is NOT a behavior change and does NOT bump
    // kWaterCAVersion: cache OFF (the default) is bit-for-bit today's code
    // path, and cache ON over unchanging terrain is bit-for-bit the same
    // again (proved in-suite: waterca_static_terrain_cache_golden_digests
    // re-derives BOTH pinned goldens with the cache enabled).
    //
    // WHY IT EXISTS. Profiling (docs/status.md, W2) puts 97-98% of the
    // hydrostatic flood's cell pops on AIR (~900K/tick on the 441-column
    // bench) and the dominant per-tick cost on the ~1us terrain `solid_`
    // query over that air shell — NOT on the flood machinery, which a prior
    // pass already made ~3x cheaper. That air shell is re-queried from
    // scratch every tick even though terrain almost never changes. This cache
    // collapses that repeat cost to a bit test.
    //
    // THE CALLER'S OBLIGATION (the whole safety contract, read this).
    // Enabling the cache asserts: "`solid_` is a pure function of position,
    // and I will tell WaterCA about EVERY change to it before the next
    // step()." A caller whose terrain can be edited at runtime (digging,
    // explosives, voxel placement, replicated world edits) MUST call
    // invalidateSolidAt / invalidateSolidRegion for every voxel whose
    // material changed — INCLUDING air->solid, not just solid->air — or call
    // invalidateSolidCache() to drop everything. A missed invalidation is a
    // SILENT DETERMINISM DIVERGENCE (water flowing through terrain that was
    // dug away, or sitting inside terrain that was placed), which is exactly
    // why this defaults to OFF: a caller opts in only once it can honour the
    // contract. Static-terrain callers (the bench, every voxel-core test)
    // satisfy it vacuously. See docs/adr/0003-hydrostatic-persistent-body.md
    // for the live-engine plumbing this still needs before the UE water
    // subsystem may enable it.
    void setSolidCacheEnabled(bool on);
    bool solidCacheEnabled() const { return solidCacheEnabled_; }

    // Drops the memo for one voxel / an inclusive voxel box / everything.
    // Cheap and always safe to over-invalidate (it is only a memo; dropping a
    // still-valid entry costs one re-query, never a wrong answer).
    void invalidateSolidAt(int64_t vx, int64_t vy, int64_t vz);
    void invalidateSolidRegion(int64_t minVx, int64_t minVy, int64_t minVz, int64_t maxVx, int64_t maxVy,
                               int64_t maxVz);
    void invalidateSolidCache() { solidCache_.clear(); }

    // Number of 8^3 bricks currently holding memoized solidity (diagnostics /
    // tests; each costs 128 bytes of masks plus the map node).
    size_t solidCacheBrickCount() const { return solidCache_.size(); }

private:
    // Per-brick memo of `solid_`: 512 "have I asked?" bits + 512 "was it
    // solid?" bits, in WaterBrick8::cellIndex order. Dense masks rather than a
    // hashed per-voxel map because the hydrostatic flood already resolves each
    // brick exactly once per tick (see hydrostaticPass's BrickCell), so a
    // pointer to this node turns every subsequent solidity question on that
    // brick into a shift-and-test with no hashing at all.
    struct SolidMaskBrick {
        std::array<uint64_t, 8> known{};
        std::array<uint64_t, 8> solid{};
    };

    // Hard bound on memo memory (128B of masks + node overhead per brick, so
    // ~16MB of masks at this cap). Overflowing simply CLEARS the whole memo —
    // safe by construction, since a memo miss only ever costs a re-query, and
    // the clear happens at a point where no cached node pointer is live (top
    // of hydrostaticPass, before any brick is resolved).
    static constexpr size_t kMaxSolidCacheBricks = 1u << 17;

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

    // Phase C — see header comment "Phase C — HYDROSTATIC". Unlike Phase
    // READ/APPLY (which is handed stepWithOrder's per-tick memoizing
    // `cachedSolid` lambda, because its 5-neighbor gathers legitimately ask
    // the SAME voxel's solidity several times per round so memoization pays
    // off), the hydrostatic flood queries each air voxel at most ONCE per
    // tick — the shared `visited` mask is checked before any solidity query,
    // so a voxel is never re-examined — so routing those queries through the
    // memo cache only adds a per-call VoxelKey hash plus an insert into a
    // map that balloons to the full air-shell size (with the rehashing that
    // implies), for ZERO dedup benefit. It therefore calls the raw `solid_`
    // callback directly (measured: ~1.8x faster on the large-pour bench than
    // going through the cache, byte-identical since memoization never changes
    // a deterministic query's answer). No longer templated: with no
    // cachedSolid lambda to inline it is a plain out-of-line member.
    void hydrostaticPass(const std::set<BrickKey, BrickKeyLess>& touched,
                         std::set<BrickKey, BrickKeyLess>& changed);

    SolidFn solid_;
    bool solidCacheEnabled_ = false;
    std::unordered_map<BrickKey, SolidMaskBrick, BrickKeyHash> solidCache_;
    WaterMap water_;
    std::set<BrickKey, BrickKeyLess> active_;
    uint64_t totalVolume_ = 0;
    size_t lastSteppedBrickCount_ = 0;
};

// ===========================================================================
// WaterMobilizer — C8 "mobilize-on-approach" (docs/cavern-design.md §5.2)
// ===========================================================================
//
// WHAT IT IS. Underground cavern lakes are generated STATIC AND IMPLICIT: a
// per-column `floodZMm` (voxelcore/caverns.h, worldgen-owned, deterministic,
// ZERO storage) says a cave-air voxel below that level "is water". Nothing is
// stored, nothing ticks, an untouched lake costs exactly nothing. The moment
// gameplay reaches one, the water it reaches must become REAL — otherwise a
// player digs into a lake and gets a frozen wall of water.
//
// This class is that conversion, and nothing else. It is terrain-free and
// worldgen-free by the same doctrine WaterCA follows: the implicit field
// arrives as a caller-supplied `ImplicitFn`, so voxel-core's water layer never
// includes caverns.h and the whole thing is testable against a synthetic lake.
//
// ---------------------------------------------------------------------------
// THE OWNERSHIP PARTITION (this is the whole correctness argument — read it)
// ---------------------------------------------------------------------------
// Every water cell in the world is owned by EXACTLY ONE of two accountants:
//
//   * the IMPLICIT FIELD, for cells in a brick that has not mobilized, or
//   * the CA, for cells in a brick that has.
//
// Mobilization is per-BRICK and one-way, recorded in `mobilizedBricks()`. The
// total water in any bounded region is therefore always
//
//       implicitVolume(region) + ca.totalVolume()      [restricted to region]
//
// and mobilizing a brick moves units from the left term to the right term in
// the same call. That is exact if and only if the CA never holds fill in a
// cell the implicit field still owns — because a cell is one byte and cannot
// carry both accountants' water at once. Note carefully that this is NOT a
// conservation bug waiting to happen but a DOUBLE-OCCUPANCY one: if CA water
// ever seeps into a still-implicit cell, then at mobilization time we would
// have to either drop the CA's units or refuse the implicit field's, and
// either way water is created or destroyed. There is no byte left to store
// the difference in.
//
// SO WE MAKE IT STRUCTURALLY IMPOSSIBLE. `makeSolidFn()` wraps the caller's
// terrain solidity query so that a still-implicit water cell reads as SOLID to
// the CA. Unmobilized lake water is a WALL. The CA physically cannot write
// fill into a cell the implicit field owns, so the partition holds by
// construction rather than by discipline, timing, or call ordering — and
// `mobilizeBrick` can then credit the FULL implicit amount into each cell
// knowing it was empty. `shortfallVolume()` is the audit of that claim and
// must be 0 forever (the tests assert it every tick).
//
// This is also what makes the per-tick BUDGET safe. Deferring a brick's
// mobilization can never leak water, because a deferred brick is still a wall.
// Water that has not been mobilized yet simply has not been given permission
// to move; it is frozen for a few ticks, never duplicated and never lost.
// Frozen is a visual lag measured in tenths of a second; duplicated is a
// broken world.
//
// ---------------------------------------------------------------------------
// HOW THE FRONT ADVANCES
// ---------------------------------------------------------------------------
// Two seeds, both required:
//
//   1. `mobilizeEditRegion` — an EDIT reaches the lake. This is the one that
//      actually starts things: digging into the wall of a static lake produces
//      no CA activity at all (there is no CA water yet), so without an edit
//      hook the front would have nothing to grow from.
//   2. `advanceFront` — CA ACTIVITY reaches the lake. Called before every
//      `ca.step()`, it mobilizes the face-neighbours of every currently active
//      brick, budget-bounded. Because a freshly mobilized brick is filled and
//      woken by `addWaterAt`, it is itself active on the next tick, so the
//      front advances one brick shell per tick, self-limiting: a lake drains
//      progressively instead of converting a million cells in one hitch.
//
// Bricks found to hold no implicit water are remembered in a transient
// negative memo (`noImplicit_`) rather than in the persisted set, so ordinary
// surface water splashing around does not grow the save file.
//
// ---------------------------------------------------------------------------
// DETERMINISM AND CLIENT/SERVER AGREEMENT
// ---------------------------------------------------------------------------
// The implicit field is a pure function of the seed, so both peers agree on
// WHERE the water is for free. What they must also agree on is WHICH bricks
// have mobilized, and that is simulation state, not worldgen: it depends on
// where players dug and when.
//
// We therefore MOBILIZE ON THE AUTHORITY ONLY. `advanceFront` is driven by the
// CA's active set, which on a client is a replication mirror rather than a
// simulation, so a client that ran its own front would drift the moment a
// packet was late. The authority mobilizes; the resulting CA fill replicates
// through the existing water-diff channel; and the mobilized brick KEYS
// replicate alongside them so the client stops drawing those bricks as
// implicit water at exactly the moment it starts drawing them as CA water.
// `markMobilized` is that inbound path (and the savegame load path): it
// records the brick without crediting anything, because the units are already
// in the replicated/persisted CA fill and crediting them again is precisely
// the duplication this class exists to prevent.
//
// Within this class every set is BrickKeyLess-ordered and every per-brick
// conversion is independent of every other, so the result is a pure function
// of (implicit field, mobilized set, active set) with no iteration-order or
// wall-clock dependence.
//
// kWaterCAVersion is NOT bumped: no tick rule changes, and a world in which
// nothing ever mobilizes is bit-for-bit a world in which this class does not
// exist. Mobilization state is SAVED alongside water state, never re-derived.
class WaterMobilizer {
public:
    // Implicit static water at a voxel, in the CA's own 0..255 fill units
    // (0 = none). In production this is the flood-level half of the predicate
    //   `cavernFloodedAt(col.cavern, vz) ? 255 : 0`
    // — see caverns.h's `cavernFloodedAt`, whose comment splits the predicate
    // exactly this way. Must be a pure, deterministic function of position and
    // seed. The other half (is this cell actually open cave air?) is supplied
    // by the `terrain` function below and applied for you in implicitFillAt,
    // so this callback never has to ask about terrain at all.
    using ImplicitFn = std::function<uint8_t(int64_t vx, int64_t vy, int64_t vz)>;

    // WHY TERRAIN IS PART OF THE IMPLICIT FIELD, NOT JUST OF THE CA.
    // Implicit water only exists in open cave air, and "open" means CURRENT
    // terrain, edits included — not the raw worldgen raster. If it meant
    // worldgen, then a player who PLACES a block into an unmobilized lake
    // would leave the implicit field still claiming 255 units in a cell that
    // is now rock; the brick would mobilize on the next edit notification and
    // the CA would refuse those units as solid, and the ledger would show a
    // shortfall for what is really just ordinary gameplay. Folding solidity in
    // here instead means a filled cell simply holds nothing, so mobilization
    // always credits exactly what it debits.
    //
    // The volume a placement destroys is then a real, intended discontinuity
    // in implicitVolume — precisely the one a placement into CA water already
    // causes in totalVolume(). Filling a hole destroys water either way; the
    // two kinds of water just behave the same.
    WaterMobilizer(ImplicitFn implicit, WaterCA::SolidFn terrain)
        : implicit_(std::move(implicit)), terrain_(std::move(terrain)) {}

    // --- the ownership partition, as queries -------------------------------

    // The implicit field's CURRENT contribution at this cell: 0 once the
    // owning brick has mobilized (the CA owns it from then on). This — never
    // the raw ImplicitFn — is the correct read for rendering and for volume
    // accounting, precisely because it respects the handover.
    uint8_t implicitFillAt(int64_t vx, int64_t vy, int64_t vz) const;

    bool isMobilized(const BrickKey& k) const { return mobilized_.count(k) != 0; }

    // Persisted state: the bricks whose implicit water the CA now owns. Only
    // bricks that actually HELD implicit water are ever recorded.
    const std::set<BrickKey, BrickKeyLess>& mobilizedBricks() const { return mobilized_; }

    // --- the wall (see "SO WE MAKE IT STRUCTURALLY IMPOSSIBLE" above) ------

    // The constructor's terrain function, wrapped so still-implicit water
    // reads as solid. This is what you hand to the WaterCA. The returned
    // function captures `this`, so the mobilizer MUST outlive the WaterCA —
    // declare it before the CA in the owning struct.
    WaterCA::SolidFn makeSolidFn() const;

    // The raw, unwrapped terrain query, for callers that need to know what the
    // ground says without the implicit-water wall on top.
    const WaterCA::SolidFn& terrainSolidFn() const { return terrain_; }

    // --- conversion --------------------------------------------------------

    // Converts one brick's implicit water into CA fill and marks it mobilized.
    // No-op (returns 0) if already mobilized or if the brick holds no implicit
    // water. Returns the units credited into the CA.
    uint32_t mobilizeBrick(WaterCA& ca, const BrickKey& k);

    // Edit hook: mobilizes every brick overlapping the inclusive voxel box,
    // grown by `kEditHaloBricks` so water on the far side of the dug face is
    // released too. Returns bricks mobilized. This is the seed — call it from
    // the same places that already call `WaterCA::wakeRegion`.
    size_t mobilizeEditRegion(WaterCA& ca, int64_t minVx, int64_t minVy, int64_t minVz,
                              int64_t maxVx, int64_t maxVy, int64_t maxVz);

    // Advances the front by up to `maxBricks` bricks: mobilizes the face
    // neighbours of every currently active CA brick. Call once before every
    // `ca.step()`. Returns bricks mobilized this call; anything over budget
    // stays queued in `pendingFrontBricks()` and is picked up next call, which
    // is safe because a queued brick is still a wall.
    static constexpr size_t kDefaultFrontBudgetBricks = 64;
    static constexpr int kEditHaloBricks = 1;
    size_t advanceFront(WaterCA& ca, size_t maxBricks = kDefaultFrontBudgetBricks);

    size_t pendingFrontBricks() const { return pending_.size(); }

    // --- ledger / audit -----------------------------------------------------

    // Units this mobilizer has moved out of the implicit field, and units it
    // actually credited into the CA. These must be EQUAL forever; their
    // difference is `shortfallVolume()`, which is nonzero only if the wall
    // invariant was broken (a cell the implicit field owned already held CA
    // fill). Tests assert 0 every tick; production can log it as a red alarm.
    uint64_t debitedVolume() const { return debited_; }
    uint64_t creditedVolume() const { return credited_; }
    uint64_t shortfallVolume() const { return debited_ - credited_; }

    // --- replication / persistence inbound --------------------------------

    // Records a brick as mobilized WITHOUT crediting anything into the CA.
    // The savegame-load and client-replication path: the units are already
    // present in the loaded/replicated CA fill, so crediting them again would
    // duplicate exactly the water this class exists to protect.
    void markMobilized(const BrickKey& k) { mobilized_.insert(k); }

    // Deterministic digest of the mobilized set, in sorted key order.
    void digest(Digest& d) const;

private:
    // The implicit field BEFORE the mobilization handover is applied: the
    // flood callback, gated on the cell actually being open cave air. This is
    // what mobilizeBrick hands over (it marks the brick first, so implicitFillAt
    // already reads 0 by then) and what implicitFillAt returns while unmobilized.
    uint8_t sourceFillAt(int64_t vx, int64_t vy, int64_t vz) const;

    // Scans `k`'s 512 cells; returns total implicit units present.
    uint64_t scanBrick(const BrickKey& k) const;

    // Bricks scanned and found to hold no implicit water. A pure negative
    // memo: it changes no answer, only the cost of re-asking, so it is never
    // persisted and may be dropped at any time (it is dropped wholesale on
    // overflow rather than evicted, same discipline as WaterCA's solid memo).
    static constexpr size_t kMaxNoImplicitBricks = 1u << 16;

    ImplicitFn implicit_;
    WaterCA::SolidFn terrain_;
    std::set<BrickKey, BrickKeyLess> mobilized_;
    std::set<BrickKey, BrickKeyLess> pending_;
    mutable std::unordered_set<BrickKey, BrickKeyHash> noImplicit_;
    uint64_t debited_ = 0;
    uint64_t credited_ = 0;
};

} // namespace vxc
