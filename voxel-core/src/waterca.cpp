#include "voxelcore/waterca.h"

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef VXC_WATER_PROFILE
#include <chrono>
#endif

namespace vxc {

#ifdef VXC_WATER_PROFILE
// See waterca.h's WaterCAProfile comment for why this is a build option.
WaterCAProfile& waterCAProfile() {
    static WaterCAProfile p;
    return p;
}
void resetWaterCAProfile() { waterCAProfile() = WaterCAProfile{}; }
namespace {
inline uint64_t profNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace
#define VXC_WP_T0(v) const uint64_t v = profNowNs()
#define VXC_WP_TV(v) uint64_t v = profNowNs()
#define VXC_WP_SET(v) (v) = profNowNs()
#define VXC_WP_ADD(field, v) waterCAProfile().field += profNowNs() - (v)
#define VXC_WP_INC(field, n) waterCAProfile().field += static_cast<uint64_t>(n)
#else
#define VXC_WP_T0(v) ((void)0)
#define VXC_WP_TV(v) ((void)0)
#define VXC_WP_SET(v) ((void)0)
#define VXC_WP_ADD(field, v) ((void)0)
#define VXC_WP_INC(field, n) ((void)0)
#endif

namespace {

constexpr int kEdge = WaterBrick8::kEdge;   // 8
constexpr int kCells = WaterBrick8::kCells; // 512

// Voxel-coordinate key for hydrostaticPass's deferred-write list -- distinct
// from BrickKey, which is truncated to int32_t brick coordinates; a voxel key
// needs the full int64_t range solid_'s own (vx,vy,vz) contract uses.
// No operator== any more. It existed for the per-tick
// unordered_map<VoxelKey, MaterialId> that Phase READ used to rebuild every
// tick -- and deleting that map IS the first of this pass's four changes, so
// the comparison went with it. VoxelKey survives only as the payload half of
// hydrostaticPass's deferred-write list, which never compares two of them.
//
// MSVC does not warn; clang's -Wunused-function -Werror does, so CI caught it
// where the local build could not.
struct VoxelKey {
    int64_t x, y, z;
};

// The 6 solidity masks Phase READ can possibly need for one active brick:
// its own, the brick below (gravity's z-1 target when the cell sits on the
// brick's floor), and its 4 lateral neighbours (+x,-x,+y,-y, in kLateralDx/Dy
// order) -- exactly the same 6-brick neighbourhood computeDesiredForBrick
// already resolves for `water`. Resolved ONCE PER TICK per active brick (the
// mask store is node-based, so these pointers stay valid across every later
// insert), which is what removes hashing from the solidity path entirely:
// inside the per-cell loop a solidity question is a pointer already in hand
// plus a shift-and-test.
struct BrickSolidMasks {
    SolidMaskBrick* self;
    SolidMaskBrick* below;
    SolidMaskBrick* lateral[4];
};

// Memoized solidity for one cell of one already-resolved brick. `miss` is the
// raw terrain callback, consulted at most once per (brick, cell) for as long
// as the mask lives -- one tick when the cross-tick memo is off, indefinitely
// (until invalidated) when it is on. Memoizing a pure query never changes its
// answer, so this is byte-identical to calling `miss` every time.
template <typename SolidMiss>
inline bool solidFromMask(SolidMaskBrick& m, int ci, int64_t vx, int64_t vy, int64_t vz, SolidMiss& miss) {
    VXC_WP_INC(readSolidLookups, 1);
    const size_t w = static_cast<size_t>(ci >> 6);
    const uint64_t bit = 1ull << (ci & 63);
    if (m.known[w] & bit) return (m.solid[w] & bit) != 0;
    const bool s = miss(vx, vy, vz);
    m.known[w] |= bit;
    if (s) m.solid[w] |= bit;
    return s;
}

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
// Cells of ONE color within an 8^3 brick (see kColorCells): 512/8.
constexpr int kColorCellsPerBrick = 64;

struct FlowScratch {
    // desired[colorIndex*kSlots + slot]: this cell's outgoing flow to that
    // slot's neighbor, computed from tick-start data only and already
    // capped so one cell's 5 slots sum to at most its own tick-start fill
    // (source-side cap, fixed priority GRAVITY,+X,-X,+Y,-Y).
    std::array<uint8_t, kColorCellsPerBrick * kSlots> desired{};
    // accepted[colorIndex*kSlots + slot]: how much of `desired` the TARGET
    // actually admitted, once every target's own capacity was resolved
    // (target-side cap, same fixed order, applied at the RECEIVING cell).
    // Written by whichever touched brick turns out to own that slot's
    // target; never read until every touched brick has been gathered.
    std::array<uint8_t, kColorCellsPerBrick * kSlots> accepted{};
    // Did Phase READ write ANY nonzero `desired` for this brick this round?
    // False is extremely common and extremely cheap to exploit: the deep
    // interior of a settled pool is all-255 cells sitting on all-255 cells, so
    // gravity offers 255-255 = 0 and every lateral neighbour is equal --
    // the whole brick has nothing to send, every round, forever. GATHER can
    // then skip the brick as a source without loading its desired[] at all
    // (which is a cache miss into ANOTHER brick's scratch), and FINALIZE can
    // skip its entire outflow scan (`accepted` is a subset of `desired`, so no
    // desired means no accepted).
    bool anyDesired = false;

    void resetForRound() {
        desired.fill(0);
        accepted.fill(0);
        anyDesired = false;
    }
};
// WHY 64 CELLS AND uint8, NOT 512 CELLS AND int16 (the shape this replaced).
//
// INDEXING. Phase READ only ever writes `desired` at cells of the ROUND's own
// color, and Phase GATHER only ever writes `accepted` back into one of those
// same source cells -- so 7 of every 8 entries of a 512-cell array were
// structurally zero for the whole round, every round. Indexing by the cell's
// position WITHIN its color (colorIndex below) instead of by cellIndex stores
// exactly the 64 that can be nonzero. This is a pure re-indexing: the same
// values live at different offsets, nothing about what is computed changes.
//
// WIDTH. Every value either array can hold is a flow amount bounded by a cell
// fill, and fills are uint8 by definition (0..255): `desired` is capped at
// min(fill, 255-neighborFill) and again at the cell's own remaining budget,
// and `accepted` is min(desired, 255-targetFill) -- so int16 was carrying a
// sign and a high byte that could never be used.
//
// WHY IT MATTERS: 10,240 bytes per active brick became 640. At the ~2150
// active bricks the 441-column pour bench reaches, the per-round working set
// went from ~22 MB (nothing but last-level cache misses, and a 22 MB memset
// per round to clear it -- 176 MB/tick over 8 rounds) to ~1.4 MB, which fits
// in cache and clears 16x faster. Measured on that bench, memo on: scratch
// reset 12.3 -> 0.9 ms/tick, GATHER 59.8 -> 22.0 ms/tick, FINALIZE 31.2 ->
// 9.0 ms/tick, per-tick scratch construction 11.7 -> 1.8 ms/tick.

// A cell's position within its own color's 64-cell stride-2 sublattice --
// i.e. its index in kColorCells[colorOf(x,y,z)]. buildColorCells appends in
// (z,y,x)-ascending order over the cells whose per-axis parities the color
// fixes, so dropping each axis' (color-determined) parity bit and packing the
// remaining 2 bits per axis reproduces that position exactly. Asserted below
// against the table itself, at compile time, so the two can never drift.
constexpr int colorIndex(int lx, int ly, int lz) { return (lx >> 1) + 4 * (ly >> 1) + 16 * (lz >> 1); }

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
// itself for outflow -- since split into a 64-cell outflow sweep and a
// 192-cell inflow sweep, see the FINALIZE loop) -- an output-preserving loop
// restructuring only: every cell any of these loops used to actually touch
// (i.e. every iteration that didn't immediately `continue` on a color
// mismatch) is still visited, in the same relative order, doing the exact
// same computation; cells that would have been skipped are now simply never
// visited, since their storage is already known-zero (resetForRound()'s
// memset, or a delta that's structurally always 0 for that cell this round --
// see gatherInflowForBrick's and the FINALIZE loop's comments for the
// per-phase color-group derivation).
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

// colorIndex() must be the exact inverse of kColorCells' own enumeration --
// FlowScratch and the per-round inflow buffers are indexed by one and walked
// by the other, so any drift would silently mis-address flows rather than
// fail to compile. Checked here against the generated table itself.
constexpr bool colorIndexMatchesColorCells() {
    for (size_t c = 0; c < 8; ++c)
        for (size_t i = 0; i < 64; ++i) {
            const ColorCell& cc = kColorCells[c][i];
            if (colorIndex(cc.x, cc.y, cc.z) != static_cast<int>(i)) return false;
        }
    return true;
}
static_assert(colorIndexMatchesColorCells(),
              "colorIndex() no longer matches kColorCells' (z,y,x)-ascending enumeration");

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
// Solidity comes in as `masks` (the caller-resolved 6-brick neighbourhood,
// see BrickSolidMasks) plus `solidMiss`, the raw terrain callback used only
// when a mask bit is not yet known. This is a pure MEMOIZATION of the same
// query: every voxel this function used to ask about is still asked about
// (once), the answers are the same, and the results are byte-identical. It
// replaces a per-TICK unordered_map<VoxelKey,MaterialId> whose every probe
// cost three splitmix64 rounds plus a hash-table walk -- measured at 134,500
// probes/tick on the 441-column pour, all of them now a shift-and-test on a
// pointer resolved once per brick per tick. When WaterCA's cross-tick memo is
// enabled the masks are the PERSISTENT ones, so the 84,279 of those probes
// that used to reach the real ~1us terrain callback every tick reach it only
// once per (voxel, terrain edit) instead.
//
// A REAL terrain-backed SolidFn (bilinear elevation + several octaves of value
// noise per call, no memoization of its own) measured at ~1us/call is the
// dominant cost of this whole engine at pour scale, dwarfing every
// per-cell-enumeration saving above -- which is why this path, not the
// enumeration, is where the memo belongs.
template <typename SolidMiss>
void computeDesiredForBrick(const BrickKey& key, const WaterMap& water, const BrickSolidMasks& masks,
                            SolidMiss&& solidMiss, int colorFilter, FlowScratch& scratch) {
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
        const int cidx = colorIndex(x, y, z); // scratch slot base (see FlowScratch)

        int remaining = fill; // source-side cap: own tick-start fill

        // Slot 0: GRAVITY (below = z-1). The cell below is in this brick
        // whenever z>0, in the z-1 brick otherwise -- the same split the
        // `belowBrick` water lookup already makes, reused for the mask.
        const int belowLz = (z > 0) ? z - 1 : kEdge - 1;
        const bool belowSolid = solidFromMask(*((z > 0) ? masks.self : masks.below),
                                              WaterBrick8::cellIndex(x, y, belowLz), vx, vy, vz - 1, solidMiss);
        uint8_t belowFill = 0;
        if (!belowSolid) {
            const WaterBrick8* b = (z > 0) ? self : belowBrick;
            belowFill = b ? b->get(x, y, belowLz) : 0;
        }
        int gFlow = 0;
        if (!belowSolid) {
            gFlow = std::min<int>(fill, 255 - belowFill);
            gFlow = std::min(gFlow, remaining);
        }
        remaining -= gFlow;
        scratch.desired[static_cast<size_t>(cidx * kSlots + SLOT_GRAVITY)] = static_cast<uint8_t>(gFlow);
        if (gFlow) scratch.anyDesired = true;

        const bool supported = belowSolid || belowFill >= 255;

        // Slots 1..4: LATERAL, fixed +x,-x,+y,-y order.
        for (int dir = 0; dir < 4; ++dir) {
            int flow = 0;
            if (supported && remaining > 0) {
                const int64_t nvx = vx + kLateralDx[dir];
                const int64_t nvy = vy + kLateralDy[dir];
                int nlx, nly, nlz;
                const bool sameBrick =
                    neighborOf(key, x, y, z, kLateralDx[dir], kLateralDy[dir], 0, nlx, nly, nlz) == key;
                if (!solidFromMask(*(sameBrick ? masks.self : masks.lateral[dir]),
                                   WaterBrick8::cellIndex(nlx, nly, nlz), nvx, nvy, vz, solidMiss)) {
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
            scratch.desired[static_cast<size_t>(cidx * kSlots + (SLOT_PX + dir))] = static_cast<uint8_t>(flow);
            if (flow) scratch.anyDesired = true;
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
// total (into `outInflow`, ALREADY pre-zeroed by the caller -- load-bearing,
// see below: this only ever WRITES nonzero admissions, so a cell it skips
// keeps the caller's zero) and writes each admitted amount back into the
// sourcing active brick's scratch.accepted (via `selfScratch`/
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

// The three inbound target groups, in the FIXED order every phase walks them:
// group 0 = the z-flip color (gravity from above), 1 = x-flip, 2 = y-flip.
// `kInflowGroups * kColorCellsPerBrick` is the whole per-brick inflow buffer:
// 192 bytes, not 512 int32s, because those 192 cells are exactly the ones that
// can receive anything this round and an admitted inflow can never exceed the
// 255-minus-current-fill budget it was capped against.
constexpr int kInflowGroups = 3;
constexpr int inflowGroupColor(int color, int group) {
    return color ^ (group == 0 ? 4 : group == 1 ? 1 : 2);
}

struct InflowBuf {
    std::array<uint8_t, kInflowGroups * kColorCellsPerBrick> v{};
    // Did GATHER admit anything at all into this brick this round? Same idea
    // (and same payoff) as FlowScratch::anyDesired: a settled brick receives
    // nothing round after round, and this lets FINALIZE skip its whole
    // 192-cell inflow scan instead of reading 192 zeroes to find that out.
    bool any = false;

    void resetForRound() {
        if (any) v.fill(0); // already all-zero otherwise, by this same invariant
        any = false;
    }
};

// Where a target cell's inbound flow comes FROM, precomputed.
//
// GATHER used to answer that with neighborOf() per (cell, slot) -- a general
// 3-axis fixup returning a BrickKey by value, then an equality compare against
// the brick's own key to decide same-brick-or-not, then colorIndex() on the
// result. At 192 candidate cells x up to 2 slots x every touched brick x 8
// rounds that was ~7M neighborOf calls per tick on the pour bench, and it was
// GATHER's single largest term. But none of it depends on the DATA: for a
// given target colour and inbound slot, "does this step leave the brick, and
// what is the source cell's index within the round's colour" is a fixed
// property of the 8^3 lattice. So it is computed once, at COMPILE time, for
// every (target colour, slot, cell) -- 8*5*64 entries, 5 KB -- and GATHER
// becomes two table loads.
//
// Indexed [targetColor][slot][i] where i is the cell's position in
// kColorCells[targetColor] (i.e. its colorIndex).
struct InboundStep {
    uint8_t srcColorIdx; // colorIndex of the source cell, in ITS own brick
    uint8_t crosses;     // 1 if the step leaves this brick (use inboundScratch)
};
constexpr std::array<std::array<std::array<InboundStep, 64>, kSlots>, 8> buildInboundTable() {
    std::array<std::array<std::array<InboundStep, 64>, kSlots>, 8> out{};
    for (size_t tc = 0; tc < 8; ++tc)
        for (size_t slot = 0; slot < kSlots; ++slot) {
            const InboundSource& s = kInbound[slot];
            for (size_t i = 0; i < 64; ++i) {
                const ColorCell& cc = kColorCells[tc][i];
                const int nx = int(cc.x) + s.dx, ny = int(cc.y) + s.dy, nz = int(cc.z) + s.dz;
                const bool crosses = nx < 0 || nx >= kEdge || ny < 0 || ny >= kEdge || nz < 0 || nz >= kEdge;
                out[tc][slot][i] = InboundStep{
                    static_cast<uint8_t>(colorIndex((nx + kEdge) & 7, (ny + kEdge) & 7, (nz + kEdge) & 7)),
                    static_cast<uint8_t>(crosses ? 1 : 0)};
            }
        }
    return out;
}
constexpr std::array<std::array<std::array<InboundStep, 64>, kSlots>, 8> kInboundTable = buildInboundTable();

void gatherInflowForBrick(const BrickKey& key, const WaterMap& water, int color, FlowScratch* selfScratch,
                          const std::array<FlowScratch*, kSlots>& inboundScratch, InflowBuf& outInflow) {
    const WaterBrick8* self = water.find(key);

    struct TargetGroup {
        const int* slots;
        int slotCount;
    };
    const TargetGroup groups[kInflowGroups] = {
        {kZFlipSlots, 1},
        {kXFlipSlots, 2},
        {kYFlipSlots, 2},
    };
    for (int gIdx = 0; gIdx < kInflowGroups; ++gIdx) {
        const TargetGroup& g = groups[gIdx];
        const size_t targetColor = static_cast<size_t>(inflowGroupColor(color, gIdx));
        const size_t outBase = static_cast<size_t>(gIdx) * kColorCellsPerBrick;
        // The 1-2 source scratch pointers for this whole group are fixed by
        // the group, not by the cell: a step either stays in this brick (self)
        // or leaves it in this slot's one fixed direction. Resolve them here
        // and skip the entire 64-cell group when neither can offer anything --
        // which is the normal state of a settled brick.
        FlowScratch* srcSelf[2];
        FlowScratch* srcCross[2];
        bool anySource = false;
        for (int gi = 0; gi < g.slotCount; ++gi) {
            const size_t i = static_cast<size_t>(g.slots[gi]);
            FlowScratch* a = (selfScratch && selfScratch->anyDesired) ? selfScratch : nullptr;
            FlowScratch* b = (inboundScratch[i] && inboundScratch[i]->anyDesired) ? inboundScratch[i] : nullptr;
            srcSelf[gi] = a;
            srcCross[gi] = b;
            if (a || b) anySource = true;
        }
        if (!anySource) continue; // group stays at the caller's pre-zeroed 0

        const WaterBrick8* selfBrick = self;
        for (size_t i = 0; i < 64; ++i) {
            // Resolve the (at most 2) inbound edges FIRST and bail before
            // touching `water` at all if none of them is offering anything --
            // the overwhelmingly common case, since a round's sources are one
            // colour out of eight and most of those have no flow to give. The
            // target's own fill is only needed to size the acceptance budget,
            // so reading it up front (as this used to) spent a water-brick
            // load per candidate cell, ~4.8M/tick on the pour bench, to
            // compute a budget that was then never used.
            size_t sidx[2];
            FlowScratch* chosen[2];
            int desired[2];
            int offered = 0;
            for (int gi = 0; gi < g.slotCount; ++gi) {
                const size_t slot = static_cast<size_t>(g.slots[gi]);
                const InboundStep& st = kInboundTable[targetColor][slot][i];
                FlowScratch* src = st.crosses ? srcCross[gi] : srcSelf[gi];
                if (!src) continue; // source brick inactive, or nothing to send
                // The source cell is of the ROUND's color (see the derivation
                // above), which is what makes colorIndex the right slot base.
                const size_t si = static_cast<size_t>(st.srcColorIdx) * kSlots +
                                  static_cast<size_t>(kInbound[slot].slot);
                const int d = src->desired[si];
                if (d <= 0) continue;
                chosen[offered] = src;
                sidx[offered] = si;
                desired[offered] = d;
                ++offered;
            }
            if (offered == 0) continue; // stays at the caller's pre-zeroed 0
            // Fixed inbound-acceptance order, unchanged: the surviving
            // candidates are still visited in kInbound order, and a candidate
            // that was skipped above would have been admitted 0 anyway.
            const ColorCell& cc = kColorCells[targetColor][i];
            const uint8_t fillStart = selfBrick ? selfBrick->get(cc.x, cc.y, cc.z) : uint8_t(0);
            int budget = 255 - int(fillStart);
            int inflow = 0;
            for (int k = 0; k < offered; ++k) {
                const int accepted = std::min(desired[k], budget);
                budget -= accepted;
                inflow += accepted;
                chosen[k]->accepted[sidx[k]] = static_cast<uint8_t>(accepted);
            }
            if (inflow) {
                outInflow.v[outBase + i] = static_cast<uint8_t>(inflow);
                outInflow.any = true;
            }
        }
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
    //
    // Each node also carries LAZILY-RESOLVED POINTERS TO ITS 6 FACE NEIGHBOURS
    // (`nbr`, in kHydroDx/Dy/Dz order, nullptr = not resolved yet). Without
    // them the flood re-hashed `cache` on every brick-crossing step: of the
    // 3072 directed cell->neighbour edges inside an 8^3 brick, 384 (12.5%)
    // leave it, so at the 868,662 cells/tick this pass pops on the 441-column
    // pour bench that was ~650,000 BrickKey hashes per tick — measured as the
    // dominant remaining cost of the flood, well ahead of the solidity queries
    // the memo already collapsed. Resolved once per (brick, direction) they
    // become a pointer load. Safe for exactly the reason the comment above
    // gives: `cache` is node-based and this pass never erases from it, so a
    // BrickCell* stays valid for the whole call.
    struct BrickCell {
        BrickKey key;
        const WaterBrick8* water; // nullptr if the brick is absent (all-empty)
        SolidMaskBrick* solidMask; // nullptr when the memo is disabled
        bool touched;
        bool queued;                     // already on the brick worklist
        std::array<uint64_t, 8> visited; // 512 bits in cellIndex order
        std::array<uint64_t, 8> pending; // discovered-but-not-yet-expanded cells
        std::array<BrickCell*, 6> nbr;   // lazily resolved, kHydroD* order
    };
    // Bound the memo BEFORE any node pointer below is taken: clearing it here
    // is safe precisely because nothing holds a SolidMaskBrick* yet.
    VXC_WP_T0(tHSetup);
    if (solidCacheEnabled_ && solidCache_.size() > kMaxSolidCacheBricks) solidCache_.clear();

    std::unordered_map<BrickKey, BrickCell, BrickKeyHash> cache;
    cache.reserve(touched.size() * 4);

    auto brickOf = [&](const BrickKey& k) -> BrickCell& {
        auto it = cache.find(k);
        if (it != cache.end()) return it->second;
        BrickCell bc;
        bc.key = k;
        bc.water = water_.find(k);
        // Node-based unordered_map: this reference stays valid across every
        // later insert into solidCache_, exactly like `cache`'s own nodes.
        bc.solidMask = solidCacheEnabled_ ? &solidCache_[k] : nullptr;
        bc.touched = touched.find(k) != touched.end();
        bc.queued = false;
        bc.visited.fill(0);
        bc.pending.fill(0);
        bc.nbr.fill(nullptr);
        return cache.emplace(k, bc).first->second;
    };
    auto neighbourBrick = [&](BrickCell& bc, int dir) -> BrickCell& {
        BrickCell*& slot = bc.nbr[static_cast<size_t>(dir)];
        if (!slot)
            slot = &brickOf(BrickKey{bc.key.x + kHydroDx[dir], bc.key.y + kHydroDy[dir],
                                     bc.key.z + kHydroDz[dir]});
        return *slot;
    };
    auto fillOf = [](const BrickCell& bc, int ci) -> uint8_t {
        return bc.water ? bc.water->cell(ci) : uint8_t(0);
    };
    auto isVisited = [](const BrickCell& bc, int ci) {
        return (bc.visited[static_cast<size_t>(ci >> 6)] >> (ci & 63)) & 1ull;
    };
    auto markVisited = [](BrickCell& bc, int ci) {
        bc.visited[static_cast<size_t>(ci >> 6)] |= (1ull << (ci & 63));
    };

    // The flood's frontier is held BRICK-MAJOR: a worklist of bricks, plus a
    // 512-bit `pending` mask per brick saying which of its cells are
    // discovered but not yet expanded. A brick is drained completely (to its
    // own local fixed point) before the worklist moves on.
    //
    // WHY, given a plain cell stack is simpler. Traversal ORDER is free here:
    // the component's cell SET, its totalVol, and its overflow decision are
    // all order-invariant, and `cells` is sorted by (z,x,y) before it is used,
    // so any order produces byte-identical output (a component that overflows
    // discards `cells` entirely, so the partial order it was filled in cannot
    // matter either). A LIFO cell stack spends that freedom badly: it walks
    // the frontier in discovery order, which on a wide flat pool hops between
    // bricks constantly, so every pop is a fresh 512-byte water brick, a fresh
    // ~200-byte BrickCell and a fresh solidity mask. Draining brick-by-brick
    // keeps all three resident for the whole 512-cell brick, and turns the
    // frontier itself from ~870,000 12-byte stack entries per tick into a few
    // thousand brick pointers plus bit sets.
    //
    // Cells within a brick are drained by word (a word IS a z-layer, since
    // cellIndex = x + 8*y + 64*z), lowest set bit first, re-scanning the 8
    // words until the brick is empty -- expanding a cell can push new pending
    // bits into a word the scan already passed, and the re-scan is 8 loads.
    std::vector<BrickCell*> brickWork;
    auto enqueue = [&brickWork](BrickCell& b, int ci) {
        b.pending[static_cast<size_t>(ci >> 6)] |= (1ull << (ci & 63));
        if (!b.queued) {
            b.queued = true;
            brickWork.push_back(&b);
        }
    };
    // Per-direction (kHydroD* order: +x,-x,+y,-y,+z,-z) cellIndex deltas for a
    // step that STAYS in the brick, and for one that LEAVES it (wrapping the
    // local coordinate from 7 to 0 or 0 to 7).
    constexpr int32_t kFloodStep[6] = {1, -1, 8, -8, 64, -64};
    constexpr int32_t kFloodCross[6] = {-7, 7, -56, 56, -448, 448};
    // Which 3-bit field of cellIndex each direction's axis lives in, and the
    // field value at which a step in that direction leaves the brick.
    constexpr int kFloodShift[6] = {0, 0, 3, 3, 6, 6};
    constexpr int kFloodEdge[6] = {7, 0, 7, 0, 7, 0};
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

#ifdef VXC_WATER_PROFILE
    uint64_t popsThisComponent = 0;
#endif
    // Expands ONE component from an already-marked-visited seed cell, filling
    // `cells` and `totalVol`; returns whether it blew the cap. The bounded-
    // through-air / unbounded-through-water rule it implements is the header's
    // "Phase C -- HYDROSTATIC" step 1:
    //
    //   * a neighbour that HOLDS WATER (fill > 0) is always explorable, from
    //     any cell, in any direction, whether or not its brick is `touched` --
    //     that is what keeps a settled arm of a body in the volume accounting;
    //   * a neighbour that is EMPTY is explorable only if (a) the step is not
    //     -z (never explore NEWLY-discovered empty space downward -- any water
    //     cell reaching this pass already settled this tick's gravity/lateral
    //     rounds, so if it had empty space below it it would not be resting
    //     there yet; going down would only ever reach dry floor BESIDE the
    //     real body, not real headroom), AND (b) the CURRENT cell is either a
    //     FULL water cell (fill==255 -- "this column is saturated, there may
    //     be pressure to rise") or an already-included empty cell (fill==0 --
    //     continuing a chain legitimately started at a full column), AND
    //     (c) the neighbour's owning brick is in `touched` (the actual
    //     "bounded to the active region" cap).
    //
    // Without the fill==255 gate on the FIRST air-ward step, any partially
    // filled water cell whose owning brick simply happens to span several
    // empty voxels above it (a brick is 8 voxels tall; that headroom is
    // `touched` purely by being the SAME brick as the water, whether or not
    // the water actually needs it) would pull that dry headroom into the
    // redistribution every tick, diluting the level and reactivating a
    // permanently growing footprint -- never a genuine pressure response, just
    // brick-granularity leakage. That was a real never-settling bug.
    auto floodComponent = [&](BrickCell& seed, int seedCi, uint64_t& totalVol) -> bool {
        bool overflowed = false;
        totalVol = 0;
        cells.clear();
        brickWork.clear();
        enqueue(seed, seedCi);
#ifdef VXC_WATER_PROFILE
        popsThisComponent = 0;
#endif
        while (!brickWork.empty()) {
            BrickCell& cBrick = *brickWork.back();
            brickWork.pop_back();
            // Deliberately stays queued==true for the whole drain: a same-brick
            // discovery then just sets a pending bit instead of pushing this
            // brick back onto the worklist, and nothing else can enqueue into
            // it while we are the only brick expanding (its cells' cross-brick
            // steps go to its neighbours, never back to itself).
            bool brickHadWork = true;
            while (brickHadWork) {
                brickHadWork = false;
                for (size_t pw = 0; pw < 8; ++pw) {
                    while (cBrick.pending[pw]) {
                        brickHadWork = true;
                        const int cci = static_cast<int>(pw) * 64 + std::countr_zero(cBrick.pending[pw]);
                        cBrick.pending[pw] &= cBrick.pending[pw] - 1;

                        const uint8_t cFill = fillOf(cBrick, cci);
                        VXC_WP_INC(hydroPops, 1);
                        VXC_WP_INC(hydroPopsAir, cFill == 0 ? 1 : 0);
#ifdef VXC_WATER_PROFILE
                        ++popsThisComponent;
#endif
                        if (!overflowed) {
                            cells.push_back(FloodCell{int64_t(cBrick.key.x) * kEdge + (cci & 7),
                                                      int64_t(cBrick.key.y) * kEdge + ((cci >> 3) & 7),
                                                      int64_t(cBrick.key.z) * kEdge + (cci >> 6), cFill});
                            totalVol += cFill;
                            if (cells.size() > kMaxHydrostaticComponentCells) overflowed = true;
                        }
                        const bool canEnterAir = cFill == 255 || cFill == 0;
                        for (int i = 0; i < 6; ++i) {
                            // Neighbour by index arithmetic: same brick unless
                            // this direction's axis field is already at the
                            // face it steps off (see kFloodStep/kFloodCross).
                            const bool crosses = ((cci >> kFloodShift[i]) & 7) == kFloodEdge[i];
                            const int nci = cci + (crosses ? kFloodCross[i] : kFloodStep[i]);
                            BrickCell& nBrick = crosses ? neighbourBrick(cBrick, i) : cBrick;
                            if (isVisited(nBrick, nci)) continue;
                            const uint8_t nFill = fillOf(nBrick, nci);
                            if (nFill == 0) {
                                if (kHydroDz[i] < 0) continue; // never explore downward into empty space
                                if (!canEnterAir) continue;    // gate: full water or already-air only
                                if (!nBrick.touched) continue; // bounded to active region
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
                                bool cellIsSolid;
                                if (nBrick.solidMask) {
                                    // Cross-tick memo hit: shift-and-test, no
                                    // terrain query at all. Same answer as
                                    // solid_ by the purity contract.
                                    const size_t w = static_cast<size_t>(nci >> 6);
                                    const uint64_t bit = 1ull << (nci & 63);
                                    if (nBrick.solidMask->known[w] & bit) {
                                        VXC_WP_INC(hydroMemoHits, 1);
                                        cellIsSolid = (nBrick.solidMask->solid[w] & bit) != 0;
                                    } else {
                                        VXC_WP_INC(hydroSolidCalls, 1);
                                        cellIsSolid =
                                            solid_(int64_t(nBrick.key.x) * kEdge + (nci & 7),
                                                   int64_t(nBrick.key.y) * kEdge + ((nci >> 3) & 7),
                                                   int64_t(nBrick.key.z) * kEdge + (nci >> 6)) != MAT_AIR;
                                        nBrick.solidMask->known[w] |= bit;
                                        if (cellIsSolid) nBrick.solidMask->solid[w] |= bit;
                                    }
                                } else {
                                    VXC_WP_INC(hydroSolidCalls, 1);
                                    cellIsSolid =
                                        solid_(int64_t(nBrick.key.x) * kEdge + (nci & 7),
                                               int64_t(nBrick.key.y) * kEdge + ((nci >> 3) & 7),
                                               int64_t(nBrick.key.z) * kEdge + (nci >> 6)) != MAT_AIR;
                                }
                                if (cellIsSolid) {
                                    markVisited(nBrick, nci);
                                    continue;
                                }
                            }
                            markVisited(nBrick, nci);
                            enqueue(nBrick, nci);
                        }
                    }
                }
            }
            cBrick.queued = false;
        }
        return overflowed;
    };
    VXC_WP_ADD(hydroSetupNs, tHSetup);
    VXC_WP_TV(tHScan);

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

                    // One component (see floodComponent above for the
                    // bounded-through-air / unbounded-through-water rule).
                    markVisited(seedBrick, ci);
                    uint64_t totalVol = 0;
                    VXC_WP_INC(hydroComponents, 1);
                    VXC_WP_ADD(hydroScanNs, tHScan);
                    VXC_WP_TV(tHFlood);
                    const bool overflowed = floodComponent(seedBrick, ci, totalVol);
                    VXC_WP_ADD(hydroFloodNs, tHFlood);
                    VXC_WP_TV(tHLevel);
#ifdef VXC_WATER_PROFILE
                    if (overflowed) {
                        ++waterCAProfile().hydroOverflowed;
                        waterCAProfile().hydroPopsOverflowed += popsThisComponent;
                    }
#endif

                    // Too big to safely handle this tick (deferred, not
                    // truncated-and-wrong -- see header comment), or a
                    // trivial single-cell "component" (just the seed,
                    // nothing reachable) that's already its own fixed
                    // point: nothing to write. (Always has water: the seed
                    // itself does, unconditionally.)
                    if (overflowed || cells.size() < 2) {
                        VXC_WP_ADD(hydroLevelNs, tHLevel);
                        VXC_WP_SET(tHScan);
                        continue;
                    }

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
                    VXC_WP_ADD(hydroLevelNs, tHLevel);
                    VXC_WP_SET(tHScan);
                }
    }
    VXC_WP_ADD(hydroScanNs, tHScan);
    VXC_WP_T0(tHApply);
    VXC_WP_INC(hydroWrites, pendingWrites.size());

    // Apply (header comment step 3): absolute targets through the normal
    // accounted-write path -- ledger, homogeneous-empty collapse, and
    // `changed` tracking all stay centralized in setFillAccounted, exactly
    // like every other write in this file. A target equal to the cell's
    // current fill is a no-op there. Deferred to here (see pendingWrites)
    // so the brick cache above never observes a mid-pass mutation of water_.
    for (const auto& [vk, target] : pendingWrites) setFillAccounted(vk.x, vk.y, vk.z, target, &changed);
    VXC_WP_ADD(hydroApplyNs, tHApply);
}

void WaterCA::step() { stepWithOrder(activeSetSnapshot()); }

void WaterCA::stepWithOrder(std::vector<BrickKey> order) {
    // Dedup defensively -- stepWithOrder accepts ANY permutation (including
    // duplicates) of the intended active set; correctness below depends
    // only on the SET of keys, never on this vector's order (see header
    // "Tick rules v1" and waterca_twophase_order_independent_and_deterministic).
    VXC_WP_T0(tSetup);
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
    VXC_WP_ADD(setupOrderNs, tSetup);
    VXC_WP_INC(ticks, 1);
    VXC_WP_INC(orderBricks, order.size());
    VXC_WP_INC(touchedBricks, touched.size());
    VXC_WP_T0(tSnap);

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
    VXC_WP_ADD(setupSnapshotNs, tSnap);
    VXC_WP_T0(tScratch);

    // Scratch/inflow storage is allocated ONCE per tick and REUSED across
    // all 8 color rounds below (only zeroed between rounds via
    // resetForRound(), never reallocated or re-inserted) -- building
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

    std::unordered_map<BrickKey, InflowBuf, BrickKeyHash> inflowMap;
    inflowMap.reserve(touched.size() * 2);
    for (const BrickKey& t : touched) inflowMap[t];

    // Per-touched-brick GATHER/FINALIZE pointer cache, resolved ONCE per
    // tick and reused by all 8 rounds below -- the other half of "stop
    // re-resolving round-INVARIANT lookups every round" (activeCache below
    // is the Phase READ counterpart).
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
        InflowBuf* inflow;
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
    VXC_WP_ADD(setupScratchNs, tScratch);

    // Memoization of solid_, per BRICK rather than per voxel.
    //
    // solid_ is a PURE function of (vx,vy,vz) that never changes within one
    // stepWithOrder() call (terrain isn't touched by water flow, and every
    // existing WaterCA contract -- determinism, order-independence -- already
    // requires callers' SolidFn be a deterministic, side-effect-free query, so
    // reusing an answer instead of re-asking changes nothing observable). And
    // solid_ queries DO legitimately repeat the same voxel: up to 5 different
    // active neighbors of one solid/air cell can each independently ask "is
    // THIS voxel solid" as part of their own gravity/lateral check (the cell
    // above asking via gravity, and up to 4 lateral neighbors each asking via
    // their own lateral check), and every one of the 8 rounds re-asks the same
    // questions the previous rounds already answered.
    //
    // WHAT CHANGED HERE AND WHY. This used to be an
    // unordered_map<VoxelKey,MaterialId> rebuilt every tick: correct, but it
    // paid three splitmix64 rounds plus a hash-table walk for EVERY solidity
    // question (measured: 134,500 probes/tick on the 441-column pour at ~2150
    // active bricks), and -- the expensive half -- it was thrown away at the
    // end of every tick, so the 84,279 probes/tick that reached the real
    // ~1us terrain callback reached it AGAIN next tick over terrain that had
    // not changed. Both problems go away by memoizing into the same dense
    // per-brick SolidMaskBrick masks Phase C already uses:
    //   * the masks for an active brick's whole 6-brick neighbourhood are
    //     resolved ONCE PER TICK (below), so an in-loop solidity question is a
    //     pointer already in hand plus a shift-and-test -- no hashing at all;
    //   * when the caller has opted into the cross-tick memo
    //     (setSolidCacheEnabled) the masks are the PERSISTENT ones, so the
    //     terrain callback is consulted once per (voxel, terrain edit) instead
    //     of once per voxel per tick.
    // With the memo OFF the masks live in `localSolid` and are discarded at
    // return, exactly like the old per-voxel map -- same number of real
    // solid_ calls as before, just without the per-query hashing. Either way
    // this is pure memoization of a pure query: same questions, same answers,
    // byte-identical results.
    //
    // Node-based unordered_map: SolidMaskBrick references stay valid across
    // every later insert (only iterators are invalidated), so the pointers
    // cached per active brick below never dangle even as the store grows
    // during the rounds.
    std::unordered_map<BrickKey, SolidMaskBrick, BrickKeyHash> localSolid;
    std::unordered_map<BrickKey, SolidMaskBrick, BrickKeyHash>& solidStore =
        solidCacheEnabled_ ? solidCache_ : localSolid;
    if (!solidCacheEnabled_) localSolid.reserve(touched.size() * 2);
    auto solidMiss = [this](int64_t vx, int64_t vy, int64_t vz) -> bool {
        VXC_WP_INC(readSolidMisses, 1);
        return solid_(vx, vy, vz) != MAT_AIR;
    };

    // Per-ACTIVE-brick round-invariant cache, the Phase READ counterpart of
    // touchedCache above: the brick's own FlowScratch pointer (so the READ
    // loop stops re-hashing scratchMap twice per brick per round) and its
    // 6-brick solidity-mask neighbourhood. Both are fixed by `order`'s
    // CONTENTS for the whole tick.
    struct ActiveBrickCache {
        BrickKey key;
        FlowScratch* scratch;
        BrickSolidMasks masks;
    };
    std::vector<ActiveBrickCache> activeCache;
    activeCache.reserve(order.size());
    for (const BrickKey& k : order) {
        ActiveBrickCache ac;
        ac.key = k;
        ac.scratch = &scratchMap[k];
        ac.masks.self = &solidStore[k];
        ac.masks.below = &solidStore[BrickKey{k.x, k.y, k.z - 1}];
        ac.masks.lateral[0] = &solidStore[BrickKey{k.x + 1, k.y, k.z}];
        ac.masks.lateral[1] = &solidStore[BrickKey{k.x - 1, k.y, k.z}];
        ac.masks.lateral[2] = &solidStore[BrickKey{k.x, k.y + 1, k.z}];
        ac.masks.lateral[3] = &solidStore[BrickKey{k.x, k.y - 1, k.z}];
        activeCache.push_back(ac);
    }

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
        VXC_WP_T0(tReset);
        for (const ActiveBrickCache& ac : activeCache) ac.scratch->resetForRound();
        VXC_WP_ADD(readResetNs, tReset);
        VXC_WP_T0(tRead);
        for (const ActiveBrickCache& ac : activeCache)
            computeDesiredForBrick(ac.key, water_, ac.masks, solidMiss, color, *ac.scratch);
        VXC_WP_ADD(readNs, tRead);

        // Phase APPLY / GATHER: visit every touched brick exactly once.
        // Order over `touched` (a sorted std::set) never affects the
        // outcome -- each (source cell, slot) is written by exactly one
        // touched brick, a structural bijection (see header), so there is
        // no write race.
#ifdef VXC_WATER_PROFILE
        // Profile builds SPLIT this loop in two so the per-brick inflow
        // zero-fill can be timed apart from the gather itself. The split is
        // output-identical (gatherInflowForBrick only ever writes its OWN
        // brick's inflow and the SOURCE bricks' `accepted`, never another
        // brick's inflow), but it does change the memory access pattern, so
        // gatherZeroNs+gatherNs is only approximately the fused loop's cost.
        // The shipped (non-profile) build keeps the loop fused.
        VXC_WP_T0(tZero);
        for (const TouchedBrickCache& tc : touchedCache) tc.inflow->resetForRound();
        VXC_WP_ADD(gatherZeroNs, tZero);
        VXC_WP_T0(tGather);
        for (const TouchedBrickCache& tc : touchedCache)
            gatherInflowForBrick(tc.key, water_, color, tc.selfScratch, tc.inboundScratch, *tc.inflow);
        VXC_WP_ADD(gatherNs, tGather);
#else
        for (const TouchedBrickCache& tc : touchedCache) {
            tc.inflow->resetForRound();
            gatherInflowForBrick(tc.key, water_, color, tc.selfScratch, tc.inboundScratch, *tc.inflow);
        }
#endif

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
        // round, and each of the 4 can only have ONE SIDE of it:
        //   * `color` itself: OUTFLOW only. `scratch->accepted` is written
        //     exclusively at color-`color` source cells (per
        //     computeDesiredForBrick/gatherInflowForBrick), and a color-`color`
        //     cell can never receive this round, because every inbound edge
        //     comes from a cell one axis-step away, whose color therefore
        //     differs from `color` -- so its inflow is structurally 0.
        //   * `color^1`/`color^2`/`color^4`: INFLOW only, symmetrically --
        //     these are exactly the 3 colors gatherInflowForBrick can write a
        //     nonzero inflow at, and none of them is `color`, so none of them
        //     has any `accepted` to spend.
        // So the old "sum 5 accepted slots then subtract from inflow" per cell
        // over all 4 groups was, for 3 of the 4 groups, summing 5 known-zero
        // bytes; and for the 4th, reading a known-zero inflow. Splitting the
        // loop in two drops both. No cell is ever in both groups (color !=
        // color^{1,2,4} always), so every cell is still visited exactly once,
        // in the same relative order, computing the same delta.
        VXC_WP_T0(tFinal);
        for (const TouchedBrickCache& tc : touchedCache) {
            const InflowBuf& inflow = *tc.inflow;
            const FlowScratch* scratch = tc.selfScratch;
            const BrickKey& t = tc.key;
            const int64_t bx = int64_t(t.x) * kEdge, by = int64_t(t.y) * kEdge, bz = int64_t(t.z) * kEdge;

            // Outflow side: this round's own color, only if this brick is
            // active (a touched-but-inactive brick has no scratch, hence no
            // outflow at all, hence delta 0 for every cell of this color) and
            // actually had something to send (see FlowScratch::anyDesired --
            // `accepted` is a subset of `desired`, so no desired means the
            // whole 64-cell scan below would find nothing but zeroes).
            if (scratch && scratch->anyDesired) {
                size_t cidx = 0;
                for (const ColorCell& cc : kColorCells[static_cast<size_t>(color)]) {
                    int outflow = 0;
                    for (int s = 0; s < kSlots; ++s) outflow += scratch->accepted[cidx * kSlots + static_cast<size_t>(s)];
                    ++cidx;
                    if (outflow == 0) continue;
                    const int64_t vx = bx + cc.x, vy = by + cc.y, vz = bz + cc.z;
                    const uint8_t fillStart = getFill(vx, vy, vz);
                    setFillAccounted(vx, vy, vz, static_cast<uint8_t>(int(fillStart) - outflow), nullptr);
                }
            }

            // Inflow side: the 3 flip colors, in gatherInflowForBrick's own
            // fixed group order (z-flip, x-flip, y-flip), which is the order
            // `inflow` is laid out in. Skipped wholesale when GATHER admitted
            // nothing into this brick this round (InflowBuf::any).
            if (!inflow.any) continue;
            for (int gIdx = 0; gIdx < kInflowGroups; ++gIdx) {
                const size_t outBase = static_cast<size_t>(gIdx) * kColorCellsPerBrick;
                size_t cidx = 0;
                for (const ColorCell& cc : kColorCells[static_cast<size_t>(inflowGroupColor(color, gIdx))]) {
                    const int in = inflow.v[outBase + cidx];
                    ++cidx;
                    if (in == 0) continue;
                    const int64_t vx = bx + cc.x, vy = by + cc.y, vz = bz + cc.z;
                    const uint8_t fillStart = getFill(vx, vy, vz);
                    setFillAccounted(vx, vy, vz, static_cast<uint8_t>(int(fillStart) + in), nullptr);
                }
            }
        }
        VXC_WP_ADD(finalizeNs, tFinal);
    }
    VXC_WP_T0(tDiff);

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
    VXC_WP_ADD(diffNs, tDiff);

    // Phase C uses the raw solid_ callback directly (NOT cachedSolid): its
    // flood queries each air voxel at most once, so the memo cache would be
    // pure overhead -- see hydrostaticPass's declaration comment in waterca.h.
    VXC_WP_T0(tHydro);
    hydrostaticPass(touched, changed);
    VXC_WP_ADD(hydroTotalNs, tHydro);
    VXC_WP_T0(tActive);

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
    VXC_WP_ADD(activeNs, tActive);
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
            // The front gate (waterca.h "the front gate"). Checked HERE as well
            // as at drain time so a permanently frozen reach beside live water
            // cannot grow `pending_` without bound -- a refused brick is simply
            // never queued. Refusing is safe for the same reason deferring is:
            // a brick the front does not mobilize is still a wall.
            if (!frontAllows(n)) {
                ++frontGateRefusals_;
                continue;
            }
            pending_.insert(n);
        }
    }

    // Drain the queue in BrickKeyLess order, budget-bounded. Deferring is safe:
    // a queued brick is still a wall, so nothing can leak into it meanwhile.
    size_t done = 0;
    while (done < maxBricks && !pending_.empty()) {
        const BrickKey k = *pending_.begin();
        pending_.erase(pending_.begin());
        // Re-checked at drain time because the gate may have closed since this
        // brick was queued (a cut logged while the queue was over budget), and
        // the gate's answer at the moment of conversion is the one that counts.
        // Dropping it costs no budget: it is a queue eviction, not a
        // mobilization, and the seed loop above will re-queue it if and when
        // the gate reopens while a neighbour is still active.
        if (!frontAllows(k)) continue;
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
