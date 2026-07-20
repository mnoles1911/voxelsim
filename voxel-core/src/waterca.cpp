#include "voxelcore/waterca.h"

#include <algorithm>
#include <array>

namespace vxc {

namespace {

constexpr int kEdge = WaterBrick8::kEdge;   // 8
constexpr int kCells = WaterBrick8::kCells; // 512

// The ONE fixed enumeration used everywhere in this file: as a cell's own
// outgoing-flow priority (source-side cap, header "Tick rules v1") AND,
// reinterpreted as "which neighbor's flow lands on me", as a target cell's
// inbound-acceptance priority (Phase APPLY / GATHER). Never reorder --
// that's the entire determinism/tie-break contract.
enum FlowSlot : int { SLOT_GRAVITY = 0, SLOT_PX = 1, SLOT_NX = 2, SLOT_PY = 3, SLOT_NY = 4, kSlots = 5 };

// Lateral neighbor offsets for slots SLOT_PX..SLOT_NY (index 0..3), matching
// v0's kDx/kDy: +x, -x, +y, -y.
constexpr int kLateralDx[4] = {1, -1, 0, 0};
constexpr int kLateralDy[4] = {0, 0, 1, -1};

// Per-active-brick scratch, built ONCE per step() call and REUSED across
// all 8 color rounds (only zeroed between rounds, never reallocated) --
// fixed-size arrays specifically so that reset is a plain memset and the
// map node carries its storage inline, instead of each of the (up to)
// several thousand active bricks' entries owning 3 separately
// heap-allocated std::vectors that would otherwise be allocated and freed
// 8 times per tick (measured: this was the dominant cost of an earlier,
// std::vector-based version of this struct -- reusing fixed storage across
// rounds/ticks is what actually delivers the two-phase rewrite's speedup,
// not the algorithm shape alone).
struct FlowScratch {
    // desired[cellIndex*kSlots + slot]: this cell's outgoing flow to that
    // slot's neighbor, computed from tick-start data only and already
    // capped so one cell's 5 slots sum to at most its own tick-start fill
    // (source-side cap, fixed priority GRAVITY,+X,-X,+Y,-Y).
    std::array<int16_t, kCells * kSlots> desired{};
    // accepted[cellIndex*kSlots + slot]: how much of `desired` the TARGET
    // actually admitted, once every target's own capacity was resolved
    // (target-side cap, same fixed order, applied at the RECEIVING cell).
    // Written by whichever touched brick turns out to own that slot's
    // target; never read until every touched brick has been gathered.
    std::array<int16_t, kCells * kSlots> accepted{};

    void resetForRound() {
        desired.fill(0);
        accepted.fill(0);
    }
};

// Resolves the brick + local cell at (lx+dx, ly+dy, lz+dz) relative to brick
// `key`, where exactly one of dx/dy/dz is nonzero (+/-1). Returns `key`
// itself (same brick) when the shifted coordinate stays in [0,8), otherwise
// the adjacent brick's key with the coordinate wrapped -- this is the
// "cached 3x3x3 neighborhood" resolution, done arithmetically instead of via
// a per-cell voxel->BrickKey floorDiv/floorMod round trip.
BrickKey neighborOf(const BrickKey& key, int lx, int ly, int lz, int dx, int dy, int dz, int& outLx,
                    int& outLy, int& outLz) {
    int nx = lx + dx, ny = ly + dy, nz = lz + dz;
    BrickKey nk = key;
    if (nx < 0) { nx += kEdge; nk.x -= 1; } else if (nx >= kEdge) { nx -= kEdge; nk.x += 1; }
    if (ny < 0) { ny += kEdge; nk.y -= 1; } else if (ny >= kEdge) { ny -= kEdge; nk.y += 1; }
    if (nz < 0) { nz += kEdge; nk.z -= 1; } else if (nz >= kEdge) { nz -= kEdge; nk.z += 1; }
    outLx = nx;
    outLy = ny;
    outLz = nz;
    return nk;
}

// 8-way (2x2x2) coloring over GLOBAL voxel coordinates: one bit per axis'
// parity, so ANY single-axis unit step (every gravity or lateral neighbor)
// flips at least one bit and therefore always changes the color. Used to
// split each tick into 8 colored rounds (see stepWithOrder) instead of a
// single fully-simultaneous (Jacobi) pass.
//
// A plain 2-color (single-bit, x^y^z parity) split was tried first and
// fixes the SIMPLE case -- a naive single-pass Jacobi update of this
// lattice-diffusion rule sits exactly AT the marginal stability boundary
// (transfer coefficient 1/2 with, worst case, all 4 neighbors requesting
// flow) and produces a persistent, never-settling checkerboard oscillation
// (found empirically: a symmetric pour never stopped stepping) -- but 2
// colors are not enough in general: a closed 4-cell loop in the lateral
// plane (e.g. (0,0),(1,0),(1,1),(0,1) at fixed z) alternates between only
// the 2 parities going around it, so a plain red-black split can still let
// such a loop trade flow in a perfect, non-flat, never-progressing cycle
// every tick (found empirically: repeating the SAME 2-round pair up to 4x
// per tick did not help -- confirming a true fixed point of that specific
// pairing, not merely slow convergence; alternating which color goes first
// tick-to-tick was tried too and made it WORSE, introducing a new
// oscillation). The 8-way coloring below assigns every cell of that same
// 4-cell loop a DIFFERENT color (each step around the loop flips a
// different bit), so no two cells of a small cycle ever move
// simultaneously off the same stale snapshot -- each round sees the
// previous rounds' results, much closer to true Gauss-Seidel, while still
// being (within each of the 8 rounds) 100% parallel and independent of
// active-set/touched-set iteration order exactly like the 2-color version.
int colorOf(int64_t vx, int64_t vy, int64_t vz) {
    return static_cast<int>((vx & 1) | ((vy & 1) << 1) | ((vz & 1) << 2));
}

// Phase READ for one active brick: fills `scratch.desired` from the CURRENT
// map state (tick-start for round 0; round-0-updated for round 1 -- see
// colorOf/stepWithOrder) for cells matching `colorFilter` only. Cells of the
// other color, and cells with fill==0, leave their slots at the
// zero-initialized default (no outflow this round).
//
// The 5 possible neighbor bricks (below, +x, -x, +y, -y) are looked up ONCE
// here, before the per-cell loop, and reused for every one of this brick's
// (up to) 512 cells -- resolving the "cached 3x3x3 neighborhood" from the
// header doctrine into O(1) hashmap lookups per BRICK instead of per CELL,
// which is the actual fix for the v0 per-cell-hashmap cost (measured:
// looking these up per-cell instead, even after fixing the earlier
// allocation-churn issue, was still the dominant remaining cost).
void computeDesiredForBrick(const BrickKey& key, const WaterMap& water, const WaterCA::SolidFn& solid,
                            int colorFilter, FlowScratch& scratch) {
    const WaterBrick8* self = water.find(key);
    if (!self) return; // drained-to-empty active brick: nothing left to send

    const WaterBrick8* belowBrick = water.find(BrickKey{key.x, key.y, key.z - 1});
    const WaterBrick8* pxBrick = water.find(BrickKey{key.x + 1, key.y, key.z});
    const WaterBrick8* nxBrick = water.find(BrickKey{key.x - 1, key.y, key.z});
    const WaterBrick8* pyBrick = water.find(BrickKey{key.x, key.y + 1, key.z});
    const WaterBrick8* nyBrick = water.find(BrickKey{key.x, key.y - 1, key.z});
    // Indexed by dir (0=+x,1=-x,2=+y,3=-y), matching kLateralDx/Dy order.
    const WaterBrick8* const lateralBrick[4] = {pxBrick, nxBrick, pyBrick, nyBrick};

    for (int z = 0; z < kEdge; ++z)
        for (int y = 0; y < kEdge; ++y)
            for (int x = 0; x < kEdge; ++x) {
                const uint8_t fill = self->get(x, y, z);
                if (fill == 0) continue;
                const int64_t vx = int64_t(key.x) * kEdge + x;
                const int64_t vy = int64_t(key.y) * kEdge + y;
                const int64_t vz = int64_t(key.z) * kEdge + z;
                if (colorOf(vx, vy, vz) != colorFilter) continue; // other color's round
                const int ci = WaterBrick8::cellIndex(x, y, z);

                int remaining = fill; // source-side cap: own tick-start fill

                // Slot 0: GRAVITY (below = z-1).
                const bool belowSolid = solid(vx, vy, vz - 1) != MAT_AIR;
                uint8_t belowFill = 0;
                if (!belowSolid) {
                    const WaterBrick8* b = (z > 0) ? self : belowBrick;
                    belowFill = b ? b->get(x, y, z > 0 ? z - 1 : kEdge - 1) : 0;
                }
                int gFlow = 0;
                if (!belowSolid) {
                    gFlow = std::min<int>(fill, 255 - belowFill);
                    gFlow = std::min(gFlow, remaining);
                }
                remaining -= gFlow;
                scratch.desired[static_cast<size_t>(ci * kSlots + SLOT_GRAVITY)] = static_cast<int16_t>(gFlow);

                const bool supported = belowSolid || belowFill >= 255;

                // Slots 1..4: LATERAL, fixed +x,-x,+y,-y order.
                for (int dir = 0; dir < 4; ++dir) {
                    int flow = 0;
                    if (supported && remaining > 0) {
                        const int64_t nvx = vx + kLateralDx[dir];
                        const int64_t nvy = vy + kLateralDy[dir];
                        if (solid(nvx, nvy, vz) == MAT_AIR) {
                            int nlx, nly, nlz;
                            const bool sameBrick =
                                neighborOf(key, x, y, z, kLateralDx[dir], kLateralDy[dir], 0, nlx, nly, nlz) == key;
                            const WaterBrick8* nBrick = sameBrick ? self : lateralBrick[dir];
                            const uint8_t nf = nBrick ? nBrick->get(nlx, nly, nlz) : 0;
                            if (fill > nf) {
                                const int diff = int(fill) - int(nf);
                                int f = diff / 2;
                                f = std::min(f, int(fill));
                                f = std::min(f, 255 - int(nf));
                                if (f > 0) flow = std::min(f, remaining);
                            }
                        }
                    }
                    remaining -= flow;
                    scratch.desired[static_cast<size_t>(ci * kSlots + (SLOT_PX + dir))] = static_cast<int16_t>(flow);
                }
            }
}

// One inbound source reference for the GATHER step: at a target cell, the
// source cell is (dx,dy,dz) away and its outgoing flow toward us lives in
// `slot` of ITS OWN scratch. Fixed order (matches SLOT_* priority exactly).
struct InboundSource {
    int dx, dy, dz, slot;
};
constexpr InboundSource kInbound[kSlots] = {
    {0, 0, 1, SLOT_GRAVITY}, // the cell above: its gravity flow lands here
    {-1, 0, 0, SLOT_PX},     // west neighbor's own +x flow lands here
    {1, 0, 0, SLOT_NX},      // east neighbor's own -x flow lands here
    {0, -1, 0, SLOT_PY},     // south neighbor's own +y flow lands here
    {0, 1, 0, SLOT_NY},      // north neighbor's own -y flow lands here
};

// Phase APPLY / GATHER for one touched brick: computes its per-cell inbound
// total (into `outInflow`, size kCells, pre-zeroed by the caller) and writes
// each admitted amount back into the sourcing active brick's
// scratch.accepted (via `scratchOf`). Reads `water` only for this brick's
// own tick-start fill (to size the per-cell budget) -- every inbound
// candidate's magnitude comes from `scratch.desired`, already computed by
// Phase READ, never from `water` again.
//
// Like computeDesiredForBrick, the (up to) 5 neighbor scratch pointers are
// resolved ONCE here, before the per-cell loop, not per cell -- this is the
// other half of the O(bricks)-not-O(cells) hashmap-lookup fix.
template <typename ScratchLookup>
void gatherInflowForBrick(const BrickKey& key, const WaterMap& water, ScratchLookup&& scratchOf,
                          std::array<int32_t, kCells>& outInflow) {
    const WaterBrick8* self = water.find(key);
    FlowScratch* selfScratch = scratchOf(key);
    // Indexed exactly like kInbound (0=gravity/above, 1=west, 2=east,
    // 3=south, 4=north).
    FlowScratch* const inboundScratch[kSlots] = {
        scratchOf(BrickKey{key.x, key.y, key.z + 1}), scratchOf(BrickKey{key.x - 1, key.y, key.z}),
        scratchOf(BrickKey{key.x + 1, key.y, key.z}), scratchOf(BrickKey{key.x, key.y - 1, key.z}),
        scratchOf(BrickKey{key.x, key.y + 1, key.z}),
    };

    for (int z = 0; z < kEdge; ++z)
        for (int y = 0; y < kEdge; ++y)
            for (int x = 0; x < kEdge; ++x) {
                const int ci = WaterBrick8::cellIndex(x, y, z);
                const uint8_t fillStart = self ? self->get(x, y, z) : 0;
                int budget = 255 - int(fillStart);
                int32_t inflow = 0;

                for (int i = 0; i < kSlots; ++i) {
                    const InboundSource& s = kInbound[i];
                    int slx, sly, slz;
                    const bool sameBrick = neighborOf(key, x, y, z, s.dx, s.dy, s.dz, slx, sly, slz) == key;
                    FlowScratch* srcScratch = sameBrick ? selfScratch : inboundScratch[i];
                    if (!srcScratch) continue; // that potential source wasn't active this tick: 0
                    const int sci = WaterBrick8::cellIndex(slx, sly, slz);
                    const int desired = srcScratch->desired[static_cast<size_t>(sci * kSlots + s.slot)];
                    if (desired <= 0) continue;
                    const int accepted = std::min(desired, budget);
                    budget -= accepted;
                    inflow += accepted;
                    srcScratch->accepted[static_cast<size_t>(sci * kSlots + s.slot)] = static_cast<int16_t>(accepted);
                }
                outInflow[static_cast<size_t>(ci)] = inflow;
            }
}

} // namespace

uint8_t WaterCA::getFill(int64_t vx, int64_t vy, int64_t vz) const {
    const BrickKey key = waterKeyForVoxel(vx, vy, vz);
    const WaterBrick8* b = water_.find(key);
    if (!b) return 0;
    return b->get(static_cast<int>(floorMod(vx, WaterBrick8::kEdge)),
                  static_cast<int>(floorMod(vy, WaterBrick8::kEdge)),
                  static_cast<int>(floorMod(vz, WaterBrick8::kEdge)));
}

void WaterCA::setFillAccounted(int64_t vx, int64_t vy, int64_t vz, uint8_t newFill,
                               std::set<BrickKey, BrickKeyLess>* changed) {
    const BrickKey key = waterKeyForVoxel(vx, vy, vz);
    const int lx = static_cast<int>(floorMod(vx, WaterBrick8::kEdge));
    const int ly = static_cast<int>(floorMod(vy, WaterBrick8::kEdge));
    const int lz = static_cast<int>(floorMod(vz, WaterBrick8::kEdge));

    WaterBrick8* b = water_.find(key);
    const uint8_t oldFill = b ? b->get(lx, ly, lz) : 0;
    if (oldFill == newFill) return; // no-op write: not a change, no ledger/activity effect

    if (!b) {
        // Only ever create a brick to store a genuine non-zero write; a
        // newFill==0 write against an absent brick already matches oldFill
        // (both zero) and returned above.
        b = &water_.getOrCreate(key);
    }
    b->set(lx, ly, lz, newFill);

    // Ledger delta (signed to allow the decrement side of a move).
    const int64_t delta = static_cast<int64_t>(newFill) - static_cast<int64_t>(oldFill);
    if (delta >= 0) totalVolume_ += static_cast<uint64_t>(delta);
    else totalVolume_ -= static_cast<uint64_t>(-delta);

    if (b->empty()) water_.erase(key); // homogeneous-empty collapse: absence, not storage

    if (changed) changed->insert(key);
}

uint32_t WaterCA::addWater(int64_t vx, int64_t vy, int64_t vz, uint32_t amount) {
    uint32_t placed = 0;
    int64_t z = vz;
    while (placed < amount) {
        if (isSolid(vx, vy, z)) break; // nowhere left in this column
        const uint8_t cur = getFill(vx, vy, z);
        const uint32_t capacity = 255u - cur;
        const uint32_t remaining = amount - placed;
        const uint32_t add = remaining < capacity ? remaining : capacity;
        if (add > 0) {
            setFillAccounted(vx, vy, z, static_cast<uint8_t>(cur + add), nullptr);
            activate(waterKeyForVoxel(vx, vy, z));
            placed += add;
        }
        ++z;
    }
    return placed;
}

uint64_t WaterCA::recomputeVolume() const {
    uint64_t sum = 0;
    for (const auto& [key, brick] : water_) sum += brick.volume();
    return sum;
}

void WaterCA::hydrostaticPass(const std::vector<BrickKey>&, std::set<BrickKey, BrickKeyLess>&) {
    // Stub (header comment "Phase C"): full hydrostatic column pressure
    // (U-bends, breach inrush) is W2-proper, not v1. Intentionally a no-op —
    // exists so the pipeline shape (read/apply -> hydrostatic) is fixed for
    // the next version to fill in without reshuffling step().
}

void WaterCA::step() { stepWithOrder(activeSetSnapshot()); }

void WaterCA::stepWithOrder(std::vector<BrickKey> order) {
    // Dedup defensively -- stepWithOrder accepts ANY permutation (including
    // duplicates) of the intended active set; correctness below depends
    // only on the SET of keys, never on this vector's order (see header
    // "Tick rules v1" and waterca_twophase_order_independent_and_deterministic).
    std::sort(order.begin(), order.end(), BrickKeyLess{});
    order.erase(std::unique(order.begin(), order.end(),
                           [](const BrickKey& a, const BrickKey& b) { return a == b; }),
               order.end());
    lastSteppedBrickCount_ = order.size();

    if (order.empty()) {
        active_.clear();
        return;
    }

    // Touched set = active bricks UNION every brick an active cell could
    // possibly target: +x,-x,+y,-y (lateral) and -z (gravity). An active
    // brick never sends flow to +z, so that neighbor is never a target.
    // Depends only on `order`'s CONTENTS (a set), computed once and reused
    // by both color rounds below.
    std::set<BrickKey, BrickKeyLess> touched(order.begin(), order.end());
    constexpr int kTargetDx[5] = {0, 1, -1, 0, 0};
    constexpr int kTargetDy[5] = {0, 0, 0, 1, -1};
    constexpr int kTargetDz[5] = {-1, 0, 0, 0, 0};
    for (const BrickKey& k : order)
        for (int i = 0; i < 5; ++i)
            touched.insert(BrickKey{k.x + kTargetDx[i], k.y + kTargetDy[i], k.z + kTargetDz[i]});

    // Snapshot every touched cell's TRUE tick-start fill before either
    // color round runs. Round 0 and round 1 each commit real writes to
    // water_ (round 1 needs to see round 0's results -- that's the whole
    // point), but a round can be reverted by the other round on cells that
    // end up net-unchanged across the full two-round tick (round 0 nudges
    // a value, round 1 nudges it right back) -- individually each write is
    // real, so feeding them straight into "changed" would mark a brick
    // active forever even though nothing about it is actually different
    // tick over tick. Comparing this snapshot against the post-both-rounds
    // state below is what makes "changed" (and therefore next tick's
    // active set, and settling) reflect NET tick-over-tick change only.
    std::unordered_map<BrickKey, std::array<uint8_t, kCells>, BrickKeyHash> initialFill;
    initialFill.reserve(touched.size() * 2);
    for (const BrickKey& t : touched) {
        const WaterBrick8* b = water_.find(t);
        std::array<uint8_t, kCells> vals{};
        if (b)
            for (int z = 0; z < kEdge; ++z)
                for (int y = 0; y < kEdge; ++y)
                    for (int x = 0; x < kEdge; ++x)
                        vals[static_cast<size_t>(WaterBrick8::cellIndex(x, y, z))] = b->get(x, y, z);
        initialFill.emplace(t, vals);
    }

    // Scratch/inflow storage is allocated ONCE per tick and REUSED across
    // all 8 color rounds below (only zeroed between rounds via
    // resetForRound()/fill(0), never reallocated or re-inserted) -- building
    // a fresh ~thousands-of-entries hashmap of heap-backed buffers 8 times
    // per tick was the dominant cost of an earlier version of this function
    // (measured: allocation/rehashing churn, not the per-cell math, was
    // eating the two-phase rewrite's expected speedup). `order`/`touched`
    // never change within one stepWithOrder call, so the same key sets are
    // valid for every round.
    std::unordered_map<BrickKey, FlowScratch, BrickKeyHash> scratchMap;
    scratchMap.reserve(order.size() * 2);
    for (const BrickKey& k : order) scratchMap[k];
    auto scratchOf = [&scratchMap](const BrickKey& k) -> FlowScratch* {
        auto it = scratchMap.find(k);
        return it == scratchMap.end() ? nullptr : &it->second;
    };

    std::unordered_map<BrickKey, std::array<int32_t, kCells>, BrickKeyHash> inflowMap;
    inflowMap.reserve(touched.size() * 2);
    for (const BrickKey& t : touched) inflowMap[t];

    // Eight colored rounds per tick (see colorOf): round c moves color-c
    // cells' outflow using data AS UPDATED BY EVERY EARLIER ROUND this tick
    // (round 0 uses true tick-start data; rounds 1-7 read straight from
    // water_, which the previous round's FINALIZE already committed to).
    // Each round is its own complete READ -> GATHER -> FINALIZE pass,
    // independent of active-set/touched-set iteration order exactly like a
    // single-color pass would be; the fixed round order 0..7 is itself part
    // of the determinism contract (not a per-tick coin flip) -- see
    // colorOf's comment for why 8 colors (not 2) are needed for correctness,
    // not just speed. Writes go straight to water_ via setFillAccounted
    // (changed=nullptr here -- see the net-diff computation after the loop
    // for the real "changed").
    for (int color = 0; color < 8; ++color) {
        // Phase READ: reset then refill every active brick's scratch (O(1)
        // lookup by key regardless of iteration order -- no allocation).
        for (const BrickKey& k : order) scratchMap[k].resetForRound();
        for (const BrickKey& k : order) computeDesiredForBrick(k, water_, solid_, color, scratchMap[k]);

        // Phase APPLY / GATHER: visit every touched brick exactly once.
        // Order over `touched` (a sorted std::set) never affects the
        // outcome -- each (source cell, slot) is written by exactly one
        // touched brick, a structural bijection (see header), so there is
        // no write race.
        for (const BrickKey& t : touched) {
            std::array<int32_t, kCells>& inflow = inflowMap[t];
            inflow.fill(0);
            gatherInflowForBrick(t, water_, scratchOf, inflow);
        }

        // Phase APPLY / FINALIZE: every active brick's `accepted` is now
        // fully populated (every one of its edges' targets has been
        // gathered above), so its true total outflow this round is known;
        // net against inflow and commit through the normal accounted-write
        // path (ledger + active-set + homogeneous-empty collapse stay
        // centralized in setFillAccounted). Committing here (inside the
        // color loop) is exactly what lets the next round see this one's
        // results.
        for (const BrickKey& t : touched) {
            const std::array<int32_t, kCells>& inflow = inflowMap.at(t);
            const FlowScratch* scratch = scratchOf(t);
            for (int z = 0; z < kEdge; ++z)
                for (int y = 0; y < kEdge; ++y)
                    for (int x = 0; x < kEdge; ++x) {
                        const int ci = WaterBrick8::cellIndex(x, y, z);
                        int outflow = 0;
                        if (scratch)
                            for (int s = 0; s < kSlots; ++s) outflow += scratch->accepted[static_cast<size_t>(ci * kSlots + s)];
                        const int32_t delta = inflow[static_cast<size_t>(ci)] - outflow;
                        if (delta == 0) continue;
                        const int64_t vx = int64_t(t.x) * kEdge + x;
                        const int64_t vy = int64_t(t.y) * kEdge + y;
                        const int64_t vz = int64_t(t.z) * kEdge + z;
                        const uint8_t fillStart = getFill(vx, vy, vz);
                        const int newFill = int(fillStart) + delta;
                        setFillAccounted(vx, vy, vz, static_cast<uint8_t>(newFill), nullptr);
                    }
        }
    }

    // Real "changed" set: a touched brick is active next tick iff its
    // content actually differs from its TRUE tick-start snapshot (not from
    // whatever happened transiently between round 0 and round 1).
    std::set<BrickKey, BrickKeyLess> changed;
    for (const BrickKey& t : touched) {
        const std::array<uint8_t, kCells>& before = initialFill.at(t);
        const WaterBrick8* b = water_.find(t);
        bool differs = false;
        for (int ci = 0; ci < kCells && !differs; ++ci) {
            const int x = ci % kEdge, y = (ci / kEdge) % kEdge, z = ci / (kEdge * kEdge);
            const uint8_t now = b ? b->get(x, y, z) : 0;
            if (now != before[static_cast<size_t>(ci)]) differs = true;
        }
        if (differs) changed.insert(t);
    }

    hydrostaticPass(order, changed);

    // Next active set = changed bricks UNION every one of their 6
    // face-neighbors (all 6, not just the 5 "outgoing target" directions
    // used for `touched` above). This is NOT optional propagation
    // padding -- it fixes a real correctness gap: a brick can be
    // completely blocked for an entire tick (e.g. gravity target still
    // full) and therefore produce zero net change itself, yet become
    // unblockable the moment that NEIGHBOR changes (the neighbor draining
    // elsewhere). Such a brick is a source or target of NOTHING this tick,
    // so it can't be reached via `changed`'s own write-driven membership,
    // but it absolutely must get another chance next tick -- otherwise it
    // is stuck forever the instant it loses one single-tick race against a
    // neighbor's own cascade (found empirically: a two-cell gravity drop
    // through a brick boundary would stall indefinitely without this).
    // All 6 directions (not just 5) because the brick ABOVE a changed
    // brick (a potential gravity SOURCE into it) must reactivate too, and
    // that's the one direction `touched`'s 5-direction target list omits.
    std::set<BrickKey, BrickKeyLess> nextActive = changed;
    constexpr int kAllDx[6] = {1, -1, 0, 0, 0, 0};
    constexpr int kAllDy[6] = {0, 0, 1, -1, 0, 0};
    constexpr int kAllDz[6] = {0, 0, 0, 0, 1, -1};
    for (const BrickKey& c : changed)
        for (int i = 0; i < 6; ++i)
            nextActive.insert(BrickKey{c.x + kAllDx[i], c.y + kAllDy[i], c.z + kAllDz[i]});

    active_ = std::move(nextActive);
}

void WaterCA::digest(Digest& d) const {
    std::vector<BrickKey> keys;
    keys.reserve(water_.size());
    for (const auto& [key, brick] : water_) keys.push_back(key);
    std::sort(keys.begin(), keys.end(), BrickKeyLess{});
    for (const BrickKey& key : keys) {
        d.i64(key.x);
        d.i64(key.y);
        d.i64(key.z);
        water_.find(key)->digest(d);
    }
}

} // namespace vxc
