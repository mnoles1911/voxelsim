#include "voxelcore/waterca.h"

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vxc {

namespace {

constexpr int kEdge = WaterBrick8::kEdge;   // 8
constexpr int kCells = WaterBrick8::kCells; // 512

// Voxel-coordinate key for the per-tick solid_ memoization cache (see
// stepWithOrder's `cachedSolid`/`solidCache`) -- distinct from BrickKey,
// which is truncated to int32_t brick coordinates; a voxel key needs the
// full int64_t range solid_'s own (vx,vy,vz) contract uses.
struct VoxelKey {
    int64_t x, y, z;
    friend bool operator==(const VoxelKey&, const VoxelKey&) = default;
};
struct VoxelKeyHash {
    size_t operator()(const VoxelKey& k) const {
        uint64_t h = splitmix64(static_cast<uint64_t>(k.x));
        h = splitmix64(h ^ static_cast<uint64_t>(k.y));
        h = splitmix64(h ^ static_cast<uint64_t>(k.z));
        return static_cast<size_t>(h);
    }
};

// Phase C (hydrostatic) full 6-neighbor offsets -- unlike `touched`'s
// gravity-only -z target direction, the hydrostatic flood fill must look
// UP too (see waterca.h "Phase C -- HYDROSTATIC" for why: finding a U-bend's
// far-arm headroom).
constexpr int kHydroDx[6] = {1, -1, 0, 0, 0, 0};
constexpr int kHydroDy[6] = {0, 0, 1, -1, 0, 0};
constexpr int kHydroDz[6] = {0, 0, 0, 0, 1, -1};

// Backstop cap on one hydrostatic component's cell count -- see waterca.h
// "Phase C -- HYDROSTATIC" for the full rationale (the water-side flood
// fill is deliberately unbounded; this is what keeps one enormous connected
// body from making a single tick's pass unbounded too).
constexpr size_t kMaxHydrostaticComponentCells = 65536;

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
constexpr int colorOf(int64_t vx, int64_t vy, int64_t vz) {
    return static_cast<int>((vx & 1) | ((vy & 1) << 1) | ((vz & 1) << 2));
}

// One cell of a color's 4x4x4 stride-2 sublattice within an 8^3 brick, as
// brick-local coordinates (0..7 each).
struct ColorCell {
    uint8_t x, y, z;
};

// kColorCells[c]: the 64 brick-local cells whose colorOf() == c (every one
// of the 8 colors partitions the 512-cell brick into exactly 64 cells since
// kEdge==8 is even on every axis). A LOCAL (x,y,z) alone already determines
// a cell's color regardless of which brick it's in: colorOf only looks at
// the low bit of each axis, and a brick's global origin (key.x*kEdge etc.)
// is always a multiple of kEdge==8, hence always even, so it never flips
// that low bit -- brick-local parity IS global parity here. Built once, at
// COMPILE time (stronger than the "static const, computed once" ask -- zero
// runtime cost at all), by literally running colorOf() over the same
// (z,y,x) full 512-cell sweep order the per-round READ/GATHER/FINALIZE
// loops used to run before this optimization, and bucketing each cell into
// its color's list -- so each list's internal order exactly matches the
// order that color's cells used to appear in when the old code scanned all
// 512 and skipped 7/8 of them, and the derivation can never drift out of
// sync with colorOf's own bit layout (the single documented source of
// truth for coloring -- see colorOf's comment reference in waterca.h "Tick
// rules v1").
//
// This is the fix for the "8 full brick scans/tick, ~1/8 productive" perf
// note (docs/status.md W2 perf note, waterca.h header): every per-round,
// color-partitioned sweep below now enumerates exactly the cells that
// round can possibly touch instead of visiting all 512 and discarding 7/8
// (READ: kColorCells[roundColor], 64 cells; GATHER: the 3 colors that can
// receive nonzero inflow from a `roundColor` source, kColorCells[roundColor
// ^ 1/2/4], 192 cells; FINALIZE: the union of GATHER's 3 plus roundColor
// itself for outflow, 256 cells) -- an output-preserving loop restructuring
// only: every cell any of these loops used to actually touch (i.e. every
// iteration that didn't immediately `continue` on a color mismatch) is
// still visited, in the same relative order, doing the exact same
// computation; cells that would have been skipped are now simply never
// visited, since their arrays are already known-zero (resetForRound()'s
// memset / the caller's inflow.fill(0) / a delta that's structurally
// always 0 for that cell this round -- see gatherInflowForBrick's and the
// FINALIZE loop's comments for the per-phase color-group derivation).
constexpr std::array<std::array<ColorCell, 64>, 8> buildColorCells() {
    std::array<std::array<ColorCell, 64>, 8> out{};
    std::array<int, 8> idx{};
    for (int z = 0; z < kEdge; ++z)
        for (int y = 0; y < kEdge; ++y)
            for (int x = 0; x < kEdge; ++x) {
                const int c = colorOf(x, y, z);
                const size_t sc = static_cast<size_t>(c);
                out[sc][static_cast<size_t>(idx[sc])] =
                    ColorCell{static_cast<uint8_t>(x), static_cast<uint8_t>(y), static_cast<uint8_t>(z)};
                ++idx[sc];
            }
    return out;
}
constexpr std::array<std::array<ColorCell, 64>, 8> kColorCells = buildColorCells();

// Phase READ for one active brick: fills `scratch.desired` from the CURRENT
// map state (tick-start for round 0; round-0-updated for round 1 -- see
// colorOf/stepWithOrder) for cells of color `colorFilter` ONLY -- enumerated
// directly via kColorCells[colorFilter] (64 cells), not by scanning all 512
// and skipping 7/8 (see kColorCells's comment). Cells of the other 7
// colors, and cells with fill==0, leave their slots at the zero-initialized
// default (no outflow this round) exactly as before -- this is a loop
// restructuring, not a behavior change.
//
// The 5 possible neighbor bricks (below, +x, -x, +y, -y) are looked up ONCE
// here, before the per-cell loop, and reused for every one of this brick's
// (up to) 512 cells -- resolving the "cached 3x3x3 neighborhood" from the
// header doctrine into O(1) hashmap lookups per BRICK instead of per CELL,
// which is the actual fix for the v0 per-cell-hashmap cost (measured:
// looking these up per-cell instead, even after fixing the earlier
// allocation-churn issue, was still the dominant remaining cost).
//
// `solid` is templated (not WaterCA::SolidFn directly) so callers can pass
// stepWithOrder's per-tick memoizing `cachedSolid` wrapper instead of the
// raw callback -- see stepWithOrder's `solidCache` comment for why: a
// REAL terrain-backed SolidFn (bilinear elevation + several octaves of
// value noise per call, no memoization of its own) measured at ~1us/call
// is the dominant cost of this whole engine at pour scale, dwarfing every
// per-cell-enumeration saving above; `solid` here is called the same
// number of times, with the same arguments, producing the same results,
// as the raw `solid_` would -- the template only changes WHICH callable
// answers each call, never what gets asked or what comes back.
template <typename SolidLookup>
void computeDesiredForBrick(const BrickKey& key, const WaterMap& water, SolidLookup&& solid, int colorFilter,
                            FlowScratch& scratch) {
    const WaterBrick8* self = water.find(key);
    if (!self) return; // drained-to-empty active brick: nothing left to send

    const WaterBrick8* belowBrick = water.find(BrickKey{key.x, key.y, key.z - 1});
    const WaterBrick8* pxBrick = water.find(BrickKey{key.x + 1, key.y, key.z});
    const WaterBrick8* nxBrick = water.find(BrickKey{key.x - 1, key.y, key.z});
    const WaterBrick8* pyBrick = water.find(BrickKey{key.x, key.y + 1, key.z});
    const WaterBrick8* nyBrick = water.find(BrickKey{key.x, key.y - 1, key.z});
    // Indexed by dir (0=+x,1=-x,2=+y,3=-y), matching kLateralDx/Dy order.
    const WaterBrick8* const lateralBrick[4] = {pxBrick, nxBrick, pyBrick, nyBrick};

    for (const ColorCell& cc : kColorCells[static_cast<size_t>(colorFilter)]) {
        const int x = cc.x, y = cc.y, z = cc.z;
        const uint8_t fill = self->get(x, y, z);
        if (fill == 0) continue;
        const int64_t vx = int64_t(key.x) * kEdge + x;
        const int64_t vy = int64_t(key.y) * kEdge + y;
        const int64_t vz = int64_t(key.z) * kEdge + z;
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
// total (into `outInflow`, size kCells, ALREADY pre-zeroed by the caller --
// load-bearing now, see below) and writes each admitted amount back into
// the sourcing active brick's scratch.accepted (via `selfScratch`/
// `inboundScratch`). Reads `water` only for this brick's own tick-start
// fill (to size the per-cell budget) -- every inbound candidate's
// magnitude comes from `scratch.desired`, already computed by Phase READ,
// never from `water` again.
//
// `selfScratch`/`inboundScratch` (this brick's own scratch, and its up-to-5
// inbound-direction neighbors' scratch, indexed exactly like kInbound) are
// caller-precomputed, NOT resolved here via a scratchOf(key) hashmap call
// per neighbor per round -- see stepWithOrder's `touchedCache` comment for
// why: unlike `water` (whose brick-level contents genuinely change
// round-to-round, so `water.find(key)` below still has to be a fresh
// per-round lookup), the SET of bricks that have scratch storage at all is
// exactly `order`'s contents, fixed for the entire tick before round 0
// ever runs -- so which of these 6 pointers is non-null, and what object
// each points at, is round-INVARIANT and only needs computing once per
// tick, reused across all 8 rounds, exactly like scratchMap/inflowMap
// themselves already are.
//
// `color` is round c's source color (the same value computeDesiredForBrick
// was just called with). This function only ever reads from a source
// cell's `desired` slot, and Phase READ only populates `desired` for cells
// of color `color` (kColorCells[color]) -- every other cell's `desired` is
// structurally all-zero this round. So a TARGET cell can only possibly
// receive nonzero inflow if it is exactly one axis-step away from SOME
// color-`color` cell, i.e. its own color is `color` with exactly one of
// the bits 1(x)/2(y)/4(z) flipped -- `color^1` (from a PX/NX-slot source,
// kInbound indices 1,2), `color^2` (PY/NY, indices 3,4), or `color^4`
// (GRAVITY, index 0). Those 3 colors are always pairwise distinct from
// each other and from `color` itself (XOR with distinct nonzero masks off
// the same base never collides), so enumerating their 3*64=192 cells (via
// kColorCells) instead of all 512 visits every cell that could possibly
// get nonzero inflow, and only those -- cells outside this union are left
// at the caller's pre-zeroed 0, which is exactly what this function would
// have computed for them anyway (every one of their 5 candidate sources
// would have hit `desired<=0` immediately). Output-preserving loop
// restructuring only.
//
// One more restriction of the exact same kind, one level deeper: for a
// GIVEN target color group, only the kInbound entries on THAT group's own
// axis can ever be nonzero -- the z-flip group (color^4) only ever has
// GRAVITY (kInbound[0]) to check; the x-flip group (color^1) only PX/NX
// (kInbound[1],[2]); the y-flip group (color^2) only PY/NY
// (kInbound[3],[4]). (Reason: a source cell's color is `color` --this
// round's only nonzero-`desired` color-- so kInbound[i]'s implied source,
// `color-group ^ (i's own axis bit)`, only equals `color` when i's axis
// matches the group's flipped axis; any other i would imply a source
// color that's never `color`, so its `desired` is structurally 0 and the
// check would always fall through the `desired<=0` continue below.)
// Checking only the applicable 1-2 slots instead of all 5 per cell is
// therefore output-preserving for the identical reason kColorCells itself
// is: every skipped check would have found nothing.
constexpr int kZFlipSlots[] = {0};
constexpr int kXFlipSlots[] = {1, 2};
constexpr int kYFlipSlots[] = {3, 4};

void gatherInflowForBrick(const BrickKey& key, const WaterMap& water, int color, FlowScratch* selfScratch,
                          const std::array<FlowScratch*, kSlots>& inboundScratch,
                          std::array<int32_t, kCells>& outInflow) {
    const WaterBrick8* self = water.find(key);

    struct TargetGroup {
        int color;
        const int* slots;
        int slotCount;
    };
    const TargetGroup groups[3] = {
        {color ^ 4, kZFlipSlots, 1},
        {color ^ 1, kXFlipSlots, 2},
        {color ^ 2, kYFlipSlots, 2},
    };
    for (const TargetGroup& g : groups)
        for (const ColorCell& cc : kColorCells[static_cast<size_t>(g.color)]) {
            const int x = cc.x, y = cc.y, z = cc.z;
            const int ci = WaterBrick8::cellIndex(x, y, z);
            const uint8_t fillStart = self ? self->get(x, y, z) : 0;
            int budget = 255 - int(fillStart);
            int32_t inflow = 0;

            for (int gi = 0; gi < g.slotCount; ++gi) {
                const int i = g.slots[gi];
                const InboundSource& s = kInbound[i];
                int slx, sly, slz;
                const bool sameBrick = neighborOf(key, x, y, z, s.dx, s.dy, s.dz, slx, sly, slz) == key;
                FlowScratch* srcScratch = sameBrick ? selfScratch : inboundScratch[static_cast<size_t>(i)];
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

uint32_t WaterCA::addWaterAt(int64_t vx, int64_t vy, int64_t vz, uint32_t amount) {
    if (amount == 0 || isSolid(vx, vy, vz)) return 0;
    const uint8_t cur = getFill(vx, vy, vz);
    const uint32_t capacity = 255u - cur;
    const uint32_t add = amount < capacity ? amount : capacity;
    if (add == 0) return 0;
    setFillAccounted(vx, vy, vz, static_cast<uint8_t>(cur + add), nullptr);
    activate(waterKeyForVoxel(vx, vy, vz));
    return add;
}

uint32_t WaterCA::removeWaterAt(int64_t vx, int64_t vy, int64_t vz, uint32_t amount) {
    if (amount == 0) return 0;
    const uint8_t cur = getFill(vx, vy, vz);
    const uint32_t take = amount < cur ? amount : cur;
    if (take == 0) return 0;
    setFillAccounted(vx, vy, vz, static_cast<uint8_t>(cur - take), nullptr);
    activate(waterKeyForVoxel(vx, vy, vz));
    return take;
}

uint64_t WaterCA::recomputeVolume() const {
    uint64_t sum = 0;
    for (const auto& [key, brick] : water_) sum += brick.volume();
    return sum;
}

// --- Cross-tick terrain-solidity memo (see waterca.h for the safety
// contract; this is plumbing only, no tick-rule content) ---------------------

void WaterCA::setSolidCacheEnabled(bool on) {
    solidCacheEnabled_ = on;
    if (!on) solidCache_.clear(); // never keep a memo we have stopped maintaining
}

void WaterCA::invalidateSolidAt(int64_t vx, int64_t vy, int64_t vz) {
    auto it = solidCache_.find(waterKeyForVoxel(vx, vy, vz));
    if (it == solidCache_.end()) return;
    const int ci = WaterBrick8::cellIndex(int(floorMod(vx, kEdge)), int(floorMod(vy, kEdge)),
                                          int(floorMod(vz, kEdge)));
    const uint64_t bit = 1ull << (ci & 63);
    it->second.known[static_cast<size_t>(ci >> 6)] &= ~bit;
    it->second.solid[static_cast<size_t>(ci >> 6)] &= ~bit;
}

void WaterCA::invalidateSolidRegion(int64_t minVx, int64_t minVy, int64_t minVz, int64_t maxVx,
                                    int64_t maxVy, int64_t maxVz) {
    if (solidCache_.empty()) return;
    if (minVx > maxVx || minVy > maxVy || minVz > maxVz) return;
    // Erase whole cached bricks rather than picking bits: a brick is the memo's
    // unit, over-invalidation is always safe (a dropped-but-still-valid entry
    // costs one re-query, never a wrong answer), and this keeps the cost
    // proportional to bricks touched instead of voxels touched. A region so
    // large that walking its bricks would cost more than a full re-memo just
    // drops everything.
    const int64_t bx0 = floorDiv(minVx, kEdge), bx1 = floorDiv(maxVx, kEdge);
    const int64_t by0 = floorDiv(minVy, kEdge), by1 = floorDiv(maxVy, kEdge);
    const int64_t bz0 = floorDiv(minVz, kEdge), bz1 = floorDiv(maxVz, kEdge);
    const uint64_t spanned = uint64_t(bx1 - bx0 + 1) * uint64_t(by1 - by0 + 1) * uint64_t(bz1 - bz0 + 1);
    if (spanned >= solidCache_.size()) {
        solidCache_.clear();
        return;
    }
    for (int64_t bz = bz0; bz <= bz1; ++bz)
        for (int64_t by = by0; by <= by1; ++by)
            for (int64_t bx = bx0; bx <= bx1; ++bx)
                solidCache_.erase(BrickKey{static_cast<int32_t>(bx), static_cast<int32_t>(by),
                                           static_cast<int32_t>(bz)});
}

// --- Terrain-edit reactivation (see waterca.h for the rule and its rationale;
// this is scheduling only — it writes no fill, so the conservation ledger
// cannot move) ---------------------------------------------------------------

size_t WaterCA::wakeRegion(int64_t minVx, int64_t minVy, int64_t minVz, int64_t maxVx, int64_t maxVy,
                           int64_t maxVz) {
    if (minVx > maxVx || minVy > maxVy || minVz > maxVz) return 0;
    if (water_.size() == 0) return 0; // nothing stores water: nothing can be woken

    // Edited voxel box -> brick box, grown by the halo (see waterca.h). Kept in
    // int64 throughout and clamped to the BrickKey int32 domain at the end, so
    // the +/-1 halo can never wrap at the coordinate extremes.
    const int64_t bx0 = floorDiv(minVx, kEdge) - kWakeHaloBricks;
    const int64_t bx1 = floorDiv(maxVx, kEdge) + kWakeHaloBricks;
    const int64_t by0 = floorDiv(minVy, kEdge) - kWakeHaloBricks;
    const int64_t by1 = floorDiv(maxVy, kEdge) + kWakeHaloBricks;
    const int64_t bz0 = floorDiv(minVz, kEdge) - kWakeHaloBricks;
    const int64_t bz1 = floorDiv(maxVz, kEdge) + kWakeHaloBricks;

    const uint64_t spanned =
        static_cast<uint64_t>(bx1 - bx0 + 1) * static_cast<uint64_t>(by1 - by0 + 1) *
        static_cast<uint64_t>(bz1 - bz0 + 1);

    size_t woken = 0;
    auto wake = [&](const BrickKey& k) {
        if (active_.insert(k).second) ++woken;
    };

    if (spanned <= static_cast<uint64_t>(water_.size())) {
        // Small edit (the overwhelmingly common case): walk the region's bricks
        // and probe the WaterMap for each. O(spanned) hash lookups.
        for (int64_t bz = bz0; bz <= bz1; ++bz)
            for (int64_t by = by0; by <= by1; ++by)
                for (int64_t bx = bx0; bx <= bx1; ++bx) {
                    if (bx < INT32_MIN || bx > INT32_MAX || by < INT32_MIN || by > INT32_MAX ||
                        bz < INT32_MIN || bz > INT32_MAX)
                        continue;
                    const BrickKey k{static_cast<int32_t>(bx), static_cast<int32_t>(by),
                                     static_cast<int32_t>(bz)};
                    if (water_.find(k) != nullptr) wake(k);
                }
    } else {
        // Region larger than the whole stored water body: cheaper to walk the
        // stored bricks and test containment. Same resulting set by
        // construction (both enumerate exactly {stored bricks} INTERSECT
        // {region bricks}); the traversal here is over an unordered_map, but
        // the RESULT is a std::set, so hash order is not observable.
        for (const auto& [k, brick] : water_) {
            (void)brick;
            if (k.x < bx0 || k.x > bx1 || k.y < by0 || k.y > by1 || k.z < bz0 || k.z > bz1) continue;
            wake(k);
        }
    }
    return woken;
}

void WaterCA::hydrostaticPass(const std::set<BrickKey, BrickKeyLess>& touched,
                              std::set<BrickKey, BrickKeyLess>& changed) {
    // --- Per-brick flood cache (W2 perf fix, docs/status.md) ----------------
    // The component partition, per-component totalVol, and bottom-up level
    // allocation below are BYTE-FOR-BYTE the same as the from-scratch flood
    // this replaced (same graph, same tie-breaks, same arithmetic); this is
    // a pure ACCESS-COST rewrite of HOW the flood reads the map, not WHAT it
    // computes. The old flood paid, for EVERY cell it popped AND every one of
    // its 6 neighbors, a full voxel->BrickKey compute + `water_` hashmap
    // find (via getFill), an `unordered_set<VoxelKey>` visited probe (three
    // splitmix64 rounds), and -- for empty neighbors -- an `inTouched`
    // set-find; profiling (status.md perf note) put that per-cell hashing,
    // not the writes, as the dominant cost of re-flooding a large settled
    // pool every tick. Here each DISTINCT brick is resolved exactly once
    // (one `water_.find`, one `touched.find`) into a cache node that carries
    // the brick's water pointer, its touched-ness, and an inline 512-bit
    // visited mask; every subsequent cell read/visit on that brick is then a
    // plain array index + bit op, no hashing. std::unordered_map guarantees
    // node references stay valid across later inserts (only iterators are
    // invalidated), so a `BrickCell&` held while resolving a neighbour brick
    // does not dangle.
    //
    // When the OPT-IN cross-tick solidity memo is enabled (waterca.h,
    // setSolidCacheEnabled), each resolved brick ALSO carries a pointer to its
    // persistent 512-bit known/solid mask pair, so the air-shell `solid_`
    // query below — profiled as the dominant cost of this whole pass — becomes
    // a shift-and-test on the second and every later tick. Memoizing a pure
    // query never changes its answer, so this is byte-identical; the caller's
    // invalidation obligation is what keeps the memo pure (see waterca.h).
    struct BrickCell {
        const WaterBrick8* water; // nullptr if the brick is absent (all-empty)
        SolidMaskBrick* solidMask; // nullptr when the memo is disabled
        bool touched;
        std::array<uint64_t, 8> visited; // 512 bits in cellIndex order
    };
    // Bound the memo BEFORE any node pointer below is taken: clearing it here
    // is safe precisely because nothing holds a SolidMaskBrick* yet.
    if (solidCacheEnabled_ && solidCache_.size() > kMaxSolidCacheBricks) solidCache_.clear();

    std::unordered_map<BrickKey, BrickCell, BrickKeyHash> cache;
    cache.reserve(touched.size() * 4);

    auto brickOf = [&](const BrickKey& k) -> BrickCell& {
        auto it = cache.find(k);
        if (it != cache.end()) return it->second;
        BrickCell bc;
        bc.water = water_.find(k);
        // Node-based unordered_map: this reference stays valid across every
        // later insert into solidCache_, exactly like `cache`'s own nodes.
        bc.solidMask = solidCacheEnabled_ ? &solidCache_[k] : nullptr;
        bc.touched = touched.find(k) != touched.end();
        bc.visited.fill(0);
        return cache.emplace(k, bc).first->second;
    };
    auto fillOf = [](const BrickCell& bc, int lx, int ly, int lz) -> uint8_t {
        return bc.water ? bc.water->get(lx, ly, lz) : uint8_t(0);
    };
    auto isVisited = [](const BrickCell& bc, int ci) {
        return (bc.visited[static_cast<size_t>(ci >> 6)] >> (ci & 63)) & 1ull;
    };
    auto markVisited = [](BrickCell& bc, int ci) {
        bc.visited[static_cast<size_t>(ci >> 6)] |= (1ull << (ci & 63));
    };

    // A flood stack entry keeps brick-local coordinates (like
    // computeDesiredForBrick's neighborOf walk) so neighbor resolution is
    // pure arithmetic -- no per-cell floorDiv/floorMod voxel->brick round
    // trip -- AND caches the already-resolved BrickCell pointer so popping a
    // cell needs no cache lookup at all, and a neighbor that stays in the
    // SAME brick (the common case for an 8^3 brick -- every interior cell,
    // and 5 of 6 faces of a boundary cell) reuses that pointer instead of
    // hashing the cache again. Node-based std::unordered_map keeps the
    // pointer valid across later inserts (only iterators are invalidated),
    // and discovery never erases, so these cached pointers never dangle.
    struct StackEntry {
        BrickCell* brick;
        BrickKey bk;
        int lx, ly, lz;
    };
    std::vector<StackEntry> stack;
    // Each discovered cell carries the fill it had at flood time. That fill
    // is still current at apply time (all writes are deferred, so nothing
    // mutates these cells between discovery and apply), which lets the level
    // step below SKIP emitting a write whenever the computed target already
    // equals the current fill -- the overwhelmingly common case for a large
    // settled pool, whose deep interior is all 255 every tick. setFillAccounted
    // early-returns on an equal old/new value and never records a no-op into
    // `changed`, so dropping those calls entirely is byte-identical, and it
    // removes the whole second O(pool) cost (a waterKeyForVoxel + water_ find
    // per interior cell, every tick) the perf note flagged alongside traversal.
    struct FloodCell {
        int64_t x, y, z;
        uint8_t fill;
    };
    std::vector<FloodCell> cells;
    cells.reserve(kMaxHydrostaticComponentCells + 1); // grow-once, reused per seed
    // Writes are DEFERRED to a single apply pass after ALL components are
    // discovered, rather than applied per-component inside the loop. This is
    // what lets the read-only brick cache above hold `water_.find` pointers
    // safely: setFillAccounted can getOrCreate/erase bricks (invalidating
    // those pointers), so no write may run while discovery is still reading.
    // Output is unchanged: distinct components never share a cell (the global
    // visited marking is preserved), so a later component's flood never reads
    // a cell an earlier component would have written; and each target is an
    // absolute value computed from the captured totalVol, independent of
    // when it is committed.
    std::vector<std::pair<VoxelKey, uint8_t>> pendingWrites;

    // Deterministic seed scan: `touched` is already a sorted std::set, and
    // each brick's 512 cells are walked in fixed (z,y,x) order -- matches
    // the fixed-order doctrine every other phase in this file follows, even
    // though (see header comment) the actual component PARTITION this
    // produces doesn't depend on scan order at all, only which cells are
    // reachable from which. Seeds are WATER cells ONLY (fill > 0) -- see
    // "empty-cell gating" below for why seeding from a bare empty cell would
    // let a pass wander through arbitrary connected dry space with no water
    // anywhere near it.
    for (const BrickKey& key : touched) {
        BrickCell& seedBrick = brickOf(key);
        if (!seedBrick.water) continue; // no water at all in this brick: no seeds
        for (int z = 0; z < kEdge; ++z)
            for (int y = 0; y < kEdge; ++y)
                for (int x = 0; x < kEdge; ++x) {
                    const int ci = WaterBrick8::cellIndex(x, y, z);
                    if (isVisited(seedBrick, ci)) continue;
                    if (seedBrick.water->get(x, y, z) == 0) continue; // not water: never a seed

                    // Bounded-through-air / unbounded-through-water flood
                    // fill (header comment "Phase C -- HYDROSTATIC" step 1).
                    //
                    // Empty-cell gating (the fix for an earlier version of
                    // this pass that never settled on an open, unwalled
                    // pool): an empty neighbor is explorable ONLY if (a) the
                    // CURRENT cell is itself either a FULL water cell
                    // (fill==255 -- "this column is saturated, there may be
                    // pressure to rise") or an already-included empty cell
                    // (fill==0 -- continuing a chain that was legitimately
                    // started at a full column), AND (b) the step is not
                    // -z (never explore NEWLY-discovered empty space
                    // downward -- any water cell reaching this pass already
                    // settled this tick's gravity/lateral rounds, so if it
                    // had empty space below it it wouldn't be resting there
                    // yet; going down would only ever reach dry floor
                    // BESIDE the real body, not real headroom), AND (c) the
                    // neighbor's owning brick is in `touched` (the actual
                    // "bounded to the active region" cap). Without the
                    // fill==255 gate on the FIRST air-ward step, any
                    // partially-filled water cell whose owning brick simply
                    // happens to span several empty voxels above it (a
                    // brick is 8 voxels tall; that headroom is `touched`
                    // purely by being the SAME brick as the water, whether
                    // or not the water actually needs it) would pull that
                    // dry headroom into the redistribution every tick,
                    // diluting the level and reactivating a permanently
                    // growing footprint -- never a genuine pressure
                    // response, just brick-granularity leakage.
                    markVisited(seedBrick, ci);
                    stack.clear();
                    stack.push_back(StackEntry{&seedBrick, key, x, y, z});
                    cells.clear();
                    bool overflowed = false;
                    uint64_t totalVol = 0;

                    while (!stack.empty()) {
                        const StackEntry c = stack.back();
                        stack.pop_back();
                        BrickCell& cBrick = *c.brick;
                        const uint8_t cFill = fillOf(cBrick, c.lx, c.ly, c.lz);
                        if (!overflowed) {
                            cells.push_back(FloodCell{int64_t(c.bk.x) * kEdge + c.lx,
                                                      int64_t(c.bk.y) * kEdge + c.ly,
                                                      int64_t(c.bk.z) * kEdge + c.lz, cFill});
                            totalVol += cFill;
                            if (cells.size() > kMaxHydrostaticComponentCells) overflowed = true;
                        }
                        const bool canEnterAir = cFill == 255 || cFill == 0;
                        for (int i = 0; i < 6; ++i) {
                            int nlx, nly, nlz;
                            const BrickKey nbk = neighborOf(c.bk, c.lx, c.ly, c.lz, kHydroDx[i], kHydroDy[i],
                                                            kHydroDz[i], nlx, nly, nlz);
                            // Same-brick neighbor (interior step): reuse the
                            // popped cell's own brick node, no cache hash.
                            BrickCell& nBrick = (nbk == c.bk) ? cBrick : brickOf(nbk);
                            const int nci = WaterBrick8::cellIndex(nlx, nly, nlz);
                            if (isVisited(nBrick, nci)) continue;
                            const uint8_t nFill = fillOf(nBrick, nlx, nly, nlz);
                            if (nFill == 0) {
                                if (kHydroDz[i] < 0) continue;   // never explore downward into empty space
                                if (!canEnterAir) continue;      // gate: full water or already-air only
                                if (!nBrick.touched) continue;   // bounded to active region
                                // Air cell passing the gates: confirm it is
                                // genuinely open (not solid terrain) before
                                // pulling it into the component. Only AIR
                                // neighbors need this query -- a WATER
                                // neighbor (nFill>0) is, by engine invariant,
                                // never inside solid terrain (addWater guards
                                // isSolid; Phase READ/APPLY only ever flows
                                // into solid()==MAT_AIR cells; hydrostatic
                                // only writes flood-included non-solid cells;
                                // and conservation would break if any water
                                // sat in a solid cell), so solid() on it is
                                // ALWAYS MAT_AIR and calling it would only add
                                // a redundant (~1us terrain) solid_ query.
                                // Matching the old code's inclusion decision
                                // exactly, just skipping the constant query.
                                bool cellIsSolid;
                                if (nBrick.solidMask) {
                                    // Cross-tick memo hit: shift-and-test, no
                                    // terrain query at all. Same answer as
                                    // solid_ by the purity contract.
                                    const size_t w = static_cast<size_t>(nci >> 6);
                                    const uint64_t bit = 1ull << (nci & 63);
                                    if (nBrick.solidMask->known[w] & bit) {
                                        cellIsSolid = (nBrick.solidMask->solid[w] & bit) != 0;
                                    } else {
                                        const int64_t nx = int64_t(nbk.x) * kEdge + nlx;
                                        const int64_t ny = int64_t(nbk.y) * kEdge + nly;
                                        const int64_t nz = int64_t(nbk.z) * kEdge + nlz;
                                        cellIsSolid = solid_(nx, ny, nz) != MAT_AIR;
                                        nBrick.solidMask->known[w] |= bit;
                                        if (cellIsSolid) nBrick.solidMask->solid[w] |= bit;
                                    }
                                } else {
                                    const int64_t nx = int64_t(nbk.x) * kEdge + nlx;
                                    const int64_t ny = int64_t(nbk.y) * kEdge + nly;
                                    const int64_t nz = int64_t(nbk.z) * kEdge + nlz;
                                    cellIsSolid = solid_(nx, ny, nz) != MAT_AIR;
                                }
                                if (cellIsSolid) {
                                    markVisited(nBrick, nci);
                                    continue;
                                }
                            }
                            markVisited(nBrick, nci);
                            stack.push_back(StackEntry{&nBrick, nbk, nlx, nly, nlz});
                        }
                    }

                    // Too big to safely handle this tick (deferred, not
                    // truncated-and-wrong -- see header comment), or a
                    // trivial single-cell "component" (just the seed,
                    // nothing reachable) that's already its own fixed
                    // point: nothing to write. (Always has water: the seed
                    // itself does, unconditionally.)
                    if (overflowed || cells.size() < 2) continue;

                    // Level computation (header comment step 2): bottom-up
                    // by z, fixed (x,y)-ascending tie-break within a layer.
                    // Only cells whose computed target DIFFERS from their
                    // current fill are emitted (see FloodCell) -- a settled
                    // interior is all no-ops and produces no writes at all.
                    std::sort(cells.begin(), cells.end(), [](const FloodCell& a, const FloodCell& b) {
                        if (a.z != b.z) return a.z < b.z;
                        if (a.x != b.x) return a.x < b.x;
                        return a.y < b.y;
                    });

                    auto emit = [&pendingWrites](const FloodCell& fc, uint8_t target) {
                        if (target != fc.fill) pendingWrites.emplace_back(VoxelKey{fc.x, fc.y, fc.z}, target);
                    };
                    uint64_t remaining = totalVol;
                    size_t idx = 0;
                    while (idx < cells.size()) {
                        size_t j = idx;
                        const int64_t layerZ = cells[idx].z;
                        while (j < cells.size() && cells[j].z == layerZ) ++j;
                        const size_t n = j - idx;
                        const uint64_t layerCap = static_cast<uint64_t>(n) * 255u;
                        if (remaining >= layerCap) {
                            for (size_t k = idx; k < j; ++k) emit(cells[k], uint8_t(255));
                            remaining -= layerCap;
                        } else if (remaining > 0) {
                            const uint64_t base = remaining / n;
                            const uint64_t rem = remaining % n; // first `rem` cells (fixed x,y order) get +1
                            for (size_t k = idx; k < j; ++k) {
                                const uint64_t v = base + ((k - idx) < rem ? 1u : 0u);
                                emit(cells[k], static_cast<uint8_t>(v));
                            }
                            remaining = 0;
                        } else {
                            for (size_t k = idx; k < j; ++k) emit(cells[k], uint8_t(0));
                        }
                        idx = j;
                    }
                }
    }

    // Apply (header comment step 3): absolute targets through the normal
    // accounted-write path -- ledger, homogeneous-empty collapse, and
    // `changed` tracking all stay centralized in setFillAccounted, exactly
    // like every other write in this file. A target equal to the cell's
    // current fill is a no-op there. Deferred to here (see pendingWrites)
    // so the brick cache above never observes a mid-pass mutation of water_.
    for (const auto& [vk, target] : pendingWrites) setFillAccounted(vk.x, vk.y, vk.z, target, &changed);
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

    // Per-touched-brick GATHER/FINALIZE pointer cache, resolved ONCE per
    // tick and reused by all 8 rounds below -- the other half of "stop
    // re-resolving round-INVARIANT lookups every round" (see solidCache
    // just below for the round-VARYING case this does NOT apply to).
    // `selfScratch`/`inboundScratch` answer "does brick K have scratch
    // storage" (K in `order`?) and "which FlowScratch object is it" --
    // both fixed by `order`'s CONTENTS, decided before round 0 ever runs
    // and never touched again (scratchMap's key set never changes, only
    // its values reset per round via resetForRound()). `inflow` is a
    // stable pointer into inflowMap's own storage for the same reason
    // (inflowMap's key set is exactly `touched`, likewise fixed for the
    // whole tick). Previously gatherInflowForBrick/the FINALIZE loop
    // re-ran up to 6 scratchMap hashmap lookups (self + 5 neighbors) AND
    // an inflowMap lookup for EVERY touched brick, EVERY round (8x
    // redundant, since none of these answers can differ round to round) --
    // profiling this pass found that redundant hashing/probing, not the
    // per-cell math, was GATHER's dominant remaining cost after the
    // color-enumeration fix above. `water.find(key)` inside
    // gatherInflowForBrick is deliberately NOT cached here: unlike scratch
    // storage, which bricks exist in `water_` and what they currently
    // contain genuinely changes round-to-round (a previous round's
    // FINALIZE can create or homogeneous-empty-collapse a brick), so that
    // lookup must stay live every round.
    struct TouchedBrickCache {
        BrickKey key;
        FlowScratch* selfScratch;
        std::array<FlowScratch*, kSlots> inboundScratch; // indexed like kInbound
        std::array<int32_t, kCells>* inflow;
    };
    std::vector<TouchedBrickCache> touchedCache;
    touchedCache.reserve(touched.size());
    for (const BrickKey& t : touched) {
        TouchedBrickCache tc;
        tc.key = t;
        tc.selfScratch = scratchOf(t);
        tc.inboundScratch = {
            scratchOf(BrickKey{t.x, t.y, t.z + 1}), scratchOf(BrickKey{t.x - 1, t.y, t.z}),
            scratchOf(BrickKey{t.x + 1, t.y, t.z}), scratchOf(BrickKey{t.x, t.y - 1, t.z}),
            scratchOf(BrickKey{t.x, t.y + 1, t.z}),
        };
        tc.inflow = &inflowMap.at(t);
        touchedCache.push_back(tc);
    }

    // Per-tick memoization of solid_: solid_ is a PURE function of
    // (vx,vy,vz) that never changes within one stepWithOrder() call
    // (terrain isn't touched by water flow, and every existing WaterCA
    // contract -- determinism, order-independence -- already requires
    // callers' SolidFn be a deterministic, side-effect-free query, so
    // reusing an answer instead of re-asking changes nothing observable).
    // Measured (profiling this pass): a REAL terrain-backed SolidFn (the
    // Amplifier's bilinear-elevation + multi-octave-noise column query,
    // recomputed from scratch every call with no memoization of its own)
    // costs on the order of 1us/call and is the dominant cost of the
    // whole engine at pour scale -- not the per-cell enumeration this
    // pass otherwise optimizes. And solid_ queries DO legitimately repeat
    // the same voxel within a tick: up to 5 different active neighbors of
    // one solid/air cell can each independently ask "is THIS voxel solid"
    // as part of their own gravity/lateral check (the cell above asking
    // via gravity, and up to 4 lateral neighbors each asking via their
    // own lateral check) -- a real, structural source of duplicate
    // queries this cache collapses to one real call. Scoped to exactly
    // ONE stepWithOrder() call (built fresh here, discarded at return,
    // same lifetime as scratchMap/inflowMap above) -- never carried
    // across ticks, so a caller whose SolidFn reflects live-edited
    // terrain (digging mid-game) is unaffected: the cache can only ever
    // go stale WITHIN a tick, and solid_ is already required not to
    // change within one.
    std::unordered_map<VoxelKey, MaterialId, VoxelKeyHash> solidCache;
    solidCache.reserve(order.size() * 16);
    auto cachedSolid = [this, &solidCache](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        const VoxelKey k{vx, vy, vz};
        const auto it = solidCache.find(k);
        if (it != solidCache.end()) return it->second;
        const MaterialId m = solid_(vx, vy, vz);
        solidCache.emplace(k, m);
        return m;
    };

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
        for (const BrickKey& k : order) computeDesiredForBrick(k, water_, cachedSolid, color, scratchMap[k]);

        // Phase APPLY / GATHER: visit every touched brick exactly once.
        // Order over `touched` (a sorted std::set) never affects the
        // outcome -- each (source cell, slot) is written by exactly one
        // touched brick, a structural bijection (see header), so there is
        // no write race.
        for (const TouchedBrickCache& tc : touchedCache) {
            tc.inflow->fill(0);
            gatherInflowForBrick(tc.key, water_, color, tc.selfScratch, tc.inboundScratch, *tc.inflow);
        }

        // Phase APPLY / FINALIZE: every active brick's `accepted` is now
        // fully populated (every one of its edges' targets has been
        // gathered above), so its true total outflow this round is known;
        // net against inflow and commit through the normal accounted-write
        // path (ledger + active-set + homogeneous-empty collapse stay
        // centralized in setFillAccounted). Committing here (inside the
        // color loop) is exactly what lets the next round see this one's
        // results.
        //
        // Only 4 of the 8 colors can possibly have a nonzero delta this
        // round: `color` itself (outflow only -- `scratch->accepted` is
        // only ever written at a color-`color` source cell, per
        // computeDesiredForBrick/gatherInflowForBrick above) and
        // `color^1`/`color^2`/`color^4` (inflow only -- see
        // gatherInflowForBrick's comment; those 3 are exactly the colors
        // `outInflow` can be nonzero at). No cell is ever in both groups
        // (color != color^{1,2,4} always), so there's no double-visit risk
        // and every cell outside this 4-color union is skipped exactly
        // where it would have hit `delta == 0` and `continue`d anyway.
        const int finalizeColors[4] = {color, color ^ 1, color ^ 2, color ^ 4};
        for (const TouchedBrickCache& tc : touchedCache) {
            const std::array<int32_t, kCells>& inflow = *tc.inflow;
            const FlowScratch* scratch = tc.selfScratch;
            const BrickKey& t = tc.key;
            for (int fc : finalizeColors)
                for (const ColorCell& cc : kColorCells[static_cast<size_t>(fc)]) {
                    const int x = cc.x, y = cc.y, z = cc.z;
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

    // Phase C uses the raw solid_ callback directly (NOT cachedSolid): its
    // flood queries each air voxel at most once, so the memo cache would be
    // pure overhead -- see hydrostaticPass's declaration comment in waterca.h.
    hydrostaticPass(touched, changed);

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

// ===========================================================================
// WaterMobilizer (see the long contract comment in waterca.h)
// ===========================================================================

namespace {

// Voxel coordinates of a water brick's (0,0,0) corner cell.
inline void brickOrigin(const BrickKey& k, int64_t& ox, int64_t& oy, int64_t& oz) {
    ox = static_cast<int64_t>(k.x) * kEdge;
    oy = static_cast<int64_t>(k.y) * kEdge;
    oz = static_cast<int64_t>(k.z) * kEdge;
}

} // namespace

uint8_t WaterMobilizer::sourceFillAt(int64_t vx, int64_t vy, int64_t vz) const {
    // Implicit water exists only in open cave air -- see the constructor's
    // "WHY TERRAIN IS PART OF THE IMPLICIT FIELD" comment for why this belongs
    // here rather than being left to the ImplicitFn or to the CA.
    if (terrain_(vx, vy, vz) != MAT_AIR) return 0;
    return implicit_(vx, vy, vz);
}

uint8_t WaterMobilizer::implicitFillAt(int64_t vx, int64_t vy, int64_t vz) const {
    // The ownership handover, in one line: once the brick has mobilized the
    // CA owns these cells and the implicit field contributes nothing.
    if (mobilized_.count(waterKeyForVoxel(vx, vy, vz)) != 0) return 0;
    return sourceFillAt(vx, vy, vz);
}

WaterCA::SolidFn WaterMobilizer::makeSolidFn() const {
    // Terrain first: it is the cheaper query and the common answer. Only for
    // genuinely open air do we ask whether the implicit field still owns this
    // cell, in which case it reads as solid -- an unmobilized lake is a WALL,
    // which is what makes the ownership partition structural (waterca.h).
    // (Inlined rather than calling implicitFillAt, to avoid asking terrain_
    // twice on the CA's hottest query.)
    return [this](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        const MaterialId m = terrain_(vx, vy, vz);
        if (m != MAT_AIR) return m;
        if (mobilized_.count(waterKeyForVoxel(vx, vy, vz)) != 0) return MAT_AIR;
        return implicit_(vx, vy, vz) != 0 ? MAT_ROCK : MAT_AIR;
    };
}

uint64_t WaterMobilizer::scanBrick(const BrickKey& k) const {
    int64_t ox = 0, oy = 0, oz = 0;
    brickOrigin(k, ox, oy, oz);
    uint64_t sum = 0;
    for (int z = 0; z < kEdge; ++z)
        for (int y = 0; y < kEdge; ++y)
            for (int x = 0; x < kEdge; ++x) sum += sourceFillAt(ox + x, oy + y, oz + z);
    return sum;
}

uint32_t WaterMobilizer::mobilizeBrick(WaterCA& ca, const BrickKey& k) {
    if (mobilized_.count(k) != 0) return 0;
    if (noImplicit_.count(k) != 0) return 0;

    if (scanBrick(k) == 0) {
        // Dry brick: nothing to hand over, and recording it would bloat the
        // persisted set for every brick ordinary surface water ever splashes
        // through. Negative memo only.
        if (noImplicit_.size() >= kMaxNoImplicitBricks) noImplicit_.clear();
        noImplicit_.insert(k);
        return 0;
    }

    // ORDER MATTERS. Mark first, so implicitFillAt() -- and therefore the wall
    // in makeSolidFn() -- reports 0 for these cells before we try to write
    // them. Then drop the CA's solidity memo for the brick, which was caching
    // "solid" for exactly those cells. Only then credit. Getting this backwards
    // would have addWaterAt refuse every cell as solid and credit nothing,
    // which shortfallVolume() would (loudly) catch.
    mobilized_.insert(k);
    recentlyMobilized_.push_back(k);
    noImplicit_.erase(k);

    int64_t ox = 0, oy = 0, oz = 0;
    brickOrigin(k, ox, oy, oz);
    ca.invalidateSolidRegion(ox, oy, oz, ox + kEdge - 1, oy + kEdge - 1, oz + kEdge - 1);

    uint32_t credited = 0;
    for (int z = 0; z < kEdge; ++z)
        for (int y = 0; y < kEdge; ++y)
            for (int x = 0; x < kEdge; ++x) {
                const uint8_t want = sourceFillAt(ox + x, oy + y, oz + z);
                if (want == 0) continue;
                debited_ += want;
                // The cell was owned by the implicit field, so it was a wall to
                // the CA and is necessarily empty: this credits the full amount.
                credited += ca.addWaterAt(ox + x, oy + y, oz + z, want);
            }
    credited_ += credited;
    return credited;
}

size_t WaterMobilizer::mobilizeEditRegion(WaterCA& ca, int64_t minVx, int64_t minVy, int64_t minVz,
                                          int64_t maxVx, int64_t maxVy, int64_t maxVz) {
    if (minVx > maxVx || minVy > maxVy || minVz > maxVz) return 0;

    const BrickKey lo = waterKeyForVoxel(minVx, minVy, minVz);
    const BrickKey hi = waterKeyForVoxel(maxVx, maxVy, maxVz);
    size_t n = 0;
    for (int32_t bz = lo.z - kEditHaloBricks; bz <= hi.z + kEditHaloBricks; ++bz)
        for (int32_t by = lo.y - kEditHaloBricks; by <= hi.y + kEditHaloBricks; ++by)
            for (int32_t bx = lo.x - kEditHaloBricks; bx <= hi.x + kEditHaloBricks; ++bx) {
                const BrickKey k{bx, by, bz};
                if (mobilizeBrick(ca, k) != 0) ++n;
            }
    return n;
}

size_t WaterMobilizer::advanceFront(WaterCA& ca, size_t maxBricks) {
    // Seed: every active brick's face neighbours. A brick only becomes active
    // once it holds (or just lost) water, so this is exactly the CA/implicit
    // contact surface, and it grows by one shell per tick.
    static constexpr int kDx[6] = {1, -1, 0, 0, 0, 0};
    static constexpr int kDy[6] = {0, 0, 1, -1, 0, 0};
    static constexpr int kDz[6] = {0, 0, 0, 0, 1, -1};

    for (const BrickKey& a : ca.activeBricks()) {
        for (int i = 0; i < 6; ++i) {
            const BrickKey n{a.x + kDx[i], a.y + kDy[i], a.z + kDz[i]};
            if (mobilized_.count(n) != 0 || noImplicit_.count(n) != 0) continue;
            pending_.insert(n);
        }
    }

    // Drain the queue in BrickKeyLess order, budget-bounded. Deferring is safe:
    // a queued brick is still a wall, so nothing can leak into it meanwhile.
    size_t done = 0;
    while (done < maxBricks && !pending_.empty()) {
        const BrickKey k = *pending_.begin();
        pending_.erase(pending_.begin());
        mobilizeBrick(ca, k);
        ++done;
    }
    return done;
}

std::vector<BrickKey> WaterMobilizer::takeRecentlyMobilized() {
    std::vector<BrickKey> out;
    out.swap(recentlyMobilized_);
    return out;
}

void WaterMobilizer::digest(Digest& d) const {
    for (const BrickKey& k : mobilized_) {
        d.i64(k.x);
        d.i64(k.y);
        d.i64(k.z);
    }
}

// ===========================================================================
// WaterState — the savegame blob (see the long contract comment in waterca.h;
// the "why this is not just a list of mobilized keys" argument lives there and
// is the whole point of this class)
// ===========================================================================

namespace {

void writeKey(ByteWriter& w, const BrickKey& k) {
    w.i32(k.x);
    w.i32(k.y);
    w.i32(k.z);
}

bool readKey(ByteReader& r, BrickKey& k) { return r.i32(k.x) && r.i32(k.y) && r.i32(k.z); }

// A u64-counted key list, required to be STRICTLY ASCENDING in BrickKeyLess
// order. Both persisted key sets are std::set<BrickKey, BrickKeyLess> on the
// write side, so this costs nothing to satisfy and buys two things: a
// re-serialized load is byte-identical, and a shuffled/duplicated blob is
// rejected instead of quietly producing a different-but-plausible world.
// Nothing is reserved off `n` before its bytes are read, so a garbage count
// fails on the first short read rather than allocating against it.
bool readKeyList(ByteReader& r, std::vector<BrickKey>& out) {
    uint64_t n = 0;
    if (!r.u64(n)) return false;
    BrickKey prev{};
    for (uint64_t i = 0; i < n; ++i) {
        BrickKey k{};
        if (!readKey(r, k)) return false;
        if (i > 0 && !BrickKeyLess{}(prev, k)) return false;
        prev = k;
        out.push_back(k);
    }
    return true;
}

WaterState::BrickFill readBrickFill(const WaterBrick8& b) {
    WaterState::BrickFill f{};
    for (int z = 0; z < kEdge; ++z)
        for (int y = 0; y < kEdge; ++y)
            for (int x = 0; x < kEdge; ++x)
                f[static_cast<size_t>(WaterBrick8::cellIndex(x, y, z))] = b.get(x, y, z);
    return f;
}

struct FillRun {
    uint8_t fill;
    uint16_t len;
};

std::vector<FillRun> runsOf(const WaterState::BrickFill& f) {
    std::vector<FillRun> runs;
    for (uint8_t v : f) {
        if (!runs.empty() && runs.back().fill == v && runs.back().len < 0xffffu)
            ++runs.back().len;
        else
            runs.push_back(FillRun{v, 1});
    }
    return runs;
}

// Encodes one brick in all three modes and writes the SMALLEST; ties break to
// the lower mode id so the choice is a pure function of the brick's contents.
// See waterca.h for what each mode is good at and
// waterca_state_sparse_encoding_beats_dense for the measurement that earned
// them their place.
void writeBrickPayload(ByteWriter& w, const WaterState::BrickFill& f) {
    size_t nonZero = 0;
    for (uint8_t v : f)
        if (v != 0) ++nonZero;
    const std::vector<FillRun> runs = runsOf(f);

    const size_t denseBytes = f.size();
    const size_t sparseBytes = 2 + 3 * nonZero;
    const size_t rleBytes = 2 + 3 * runs.size();

    if (denseBytes <= sparseBytes && denseBytes <= rleBytes) {
        w.u8(WaterState::kDense);
        for (uint8_t v : f) w.u8(v);
        return;
    }
    if (sparseBytes <= rleBytes) {
        w.u8(WaterState::kSparse);
        w.u16(static_cast<uint16_t>(nonZero));
        for (size_t ci = 0; ci < f.size(); ++ci) {
            if (f[ci] == 0) continue;
            w.u16(static_cast<uint16_t>(ci));
            w.u8(f[ci]);
        }
        return;
    }
    w.u8(WaterState::kRle);
    w.u16(static_cast<uint16_t>(runs.size()));
    for (const FillRun& run : runs) {
        w.u8(run.fill);
        w.u16(run.len);
    }
}

bool readBrickPayload(ByteReader& r, WaterState::BrickFill& f) {
    f.fill(0);
    uint8_t mode = 0;
    if (!r.u8(mode)) return false;

    if (mode == WaterState::kDense) {
        for (size_t i = 0; i < f.size(); ++i)
            if (!r.u8(f[i])) return false;
        return true;
    }
    if (mode == WaterState::kSparse) {
        uint16_t n = 0;
        if (!r.u16(n)) return false;
        uint32_t prev = 0;
        for (uint16_t i = 0; i < n; ++i) {
            uint16_t cell = 0;
            uint8_t v = 0;
            if (!r.u16(cell) || !r.u8(v)) return false;
            if (cell >= f.size()) return false;
            if (v == 0) return false;                    // never canonical: 0 is the default
            if (i > 0 && cell <= prev) return false;     // sorted, unique
            prev = cell;
            f[static_cast<size_t>(cell)] = v;
        }
        return true;
    }
    if (mode == WaterState::kRle) {
        uint16_t n = 0;
        if (!r.u16(n)) return false;
        size_t cell = 0;
        for (uint16_t i = 0; i < n; ++i) {
            uint8_t v = 0;
            uint16_t len = 0;
            if (!r.u8(v) || !r.u16(len)) return false;
            if (len == 0) return false;                  // never canonical: empty run
            if (static_cast<size_t>(len) > f.size() - cell) return false;
            for (uint16_t j = 0; j < len; ++j) f[cell++] = v;
        }
        return cell == f.size();                         // must cover exactly 512 cells
    }
    return false;
}

} // namespace

void WaterState::serialize(const WaterCA& ca, const WaterMobilizer& mob,
                           std::vector<uint8_t>& out) {
    ByteWriter w(out);
    w.u32(kMagic);
    w.u32(kFormatVersion);
    w.u32(kWaterCAVersion);
    w.u64(ca.totalVolume());

    // The two key sets below are already BrickKeyLess-ordered (std::set), so
    // they need no sorting at all. The fill map is the one exception: WaterMap
    // is an unordered_map, so its keys get one explicit sort here -- exactly
    // what WaterCA::digest does, for exactly the same reason.
    std::vector<BrickKey> keys;
    keys.reserve(ca.bricks().size());
    for (const auto& entry : ca.bricks()) keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end(), BrickKeyLess{});

    w.u64(keys.size());
    for (const BrickKey& k : keys) {
        writeKey(w, k);
        writeBrickPayload(w, readBrickFill(*ca.bricks().find(k)));
    }

    w.u64(ca.activeBricks().size());
    for (const BrickKey& k : ca.activeBricks()) writeKey(w, k);

    w.u64(mob.mobilizedBricks().size());
    for (const BrickKey& k : mob.mobilizedBricks()) writeKey(w, k);
}

std::optional<WaterState> WaterState::parse(const uint8_t* data, size_t size) {
    ByteReader r(data, size);
    uint32_t magic = 0, fmt = 0, caVersion = 0;
    if (!r.u32(magic) || magic != kMagic) return std::nullopt;
    if (!r.u32(fmt) || fmt != kFormatVersion) return std::nullopt;
    // EXACT match, not a range: fill produced under different tick rules is a
    // different simulation state, and per docs/determinism.md that is what the
    // constant exists to signal. Refusing here leaves the caller with a world
    // whose water reverts to implicit -- degraded, but coherent.
    if (!r.u32(caVersion) || caVersion != kWaterCAVersion) return std::nullopt;

    WaterState s;
    if (!r.u64(s.totalVolume)) return std::nullopt;

    uint64_t brickCount = 0;
    if (!r.u64(brickCount)) return std::nullopt;
    uint64_t sum = 0;
    BrickKey prev{};
    for (uint64_t i = 0; i < brickCount; ++i) {
        BrickKey k{};
        if (!readKey(r, k)) return std::nullopt;
        if (i > 0 && !BrickKeyLess{}(prev, k)) return std::nullopt; // strictly ascending
        prev = k;
        BrickFill f{};
        if (!readBrickPayload(r, f)) return std::nullopt;
        uint64_t brickSum = 0;
        for (uint8_t v : f) brickSum += static_cast<uint64_t>(v);
        // An all-zero brick is never stored (homogeneous-empty collapse ==
        // absence), so its presence means the blob did not come from
        // serialize() and nothing about it should be trusted.
        if (brickSum == 0) return std::nullopt;
        sum += brickSum;
        s.bricks.push_back({k, f});
    }
    // Integrity: the ledger we just re-derived from the decoded fills must
    // equal the one the save recorded. This is the loud half of "a save/load
    // cycle must not silently lose water" -- the quiet half is the round-trip
    // test, and this one also fires in production.
    if (sum != s.totalVolume) return std::nullopt;

    if (!readKeyList(r, s.active)) return std::nullopt;
    if (!readKeyList(r, s.mobilized)) return std::nullopt;
    if (!r.atEnd()) return std::nullopt; // trailing bytes: truncated append or garbage
    return s;
}

bool WaterState::applyTo(WaterCA& ca, WaterMobilizer& mob) const {
    // A non-empty target would leave behind stale cells this blob never
    // mentions -- a silent merge, not a load. Refuse before writing anything.
    if (ca.storedBrickCount() != 0 || ca.totalVolume() != 0 || ca.activeBrickCount() != 0)
        return false;
    if (!mob.mobilizedBricks().empty()) return false;

    // FILLS FIRST (waterca.h explains why the order is written down even
    // though the end state is order-independent). setReplicatedFill writes
    // state directly and never consults solidity, so it is untroubled by
    // makeSolidFn() still reporting these cells as implicit-water WALL at this
    // point -- which is precisely what lets the fill land before the wall is
    // lifted, instead of after.
    for (const auto& entry : bricks) {
        const BrickKey& k = entry.first;
        const BrickFill& f = entry.second;
        const int64_t ox = static_cast<int64_t>(k.x) * kEdge;
        const int64_t oy = static_cast<int64_t>(k.y) * kEdge;
        const int64_t oz = static_cast<int64_t>(k.z) * kEdge;
        for (int z = 0; z < kEdge; ++z)
            for (int y = 0; y < kEdge; ++y)
                for (int x = 0; x < kEdge; ++x) {
                    const uint8_t v = f[static_cast<size_t>(WaterBrick8::cellIndex(x, y, z))];
                    if (v != 0) ca.setReplicatedFill(ox + x, oy + y, oz + z, v);
                }
    }

    // Scheduling next: restores mid-flow water as mid-flow. Writes nothing.
    for (const BrickKey& k : active) ca.markActive(k);

    // The wall comes down LAST. markMobilized credits nothing -- the units are
    // already in the fill loaded above, which is the precondition its doc
    // comment states and the one this class exists to actually satisfy.
    for (const BrickKey& k : mobilized) mob.markMobilized(k);
    return true;
}

bool WaterState::load(const uint8_t* data, size_t size, WaterCA& ca, WaterMobilizer& mob) {
    const std::optional<WaterState> s = parse(data, size);
    if (!s) return false;
    return s->applyTo(ca, mob);
}

} // namespace vxc
