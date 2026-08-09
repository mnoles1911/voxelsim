#pragma once
// THE BASIN VOLUME LEDGER -- how much water a lake holds right now, as an
// integer, and what surface level that puts it at.
//
// docs/water-rearchitecture-plan-2026-08-09.md Phase 2, and §4/§5's authority
// split: **authority = scalar hydrology, particles are presentation.** The
// baked basin table (tilestore.h `BasinEntry`, SECTION_BASIN_TABLE, bake_ver 8)
// ships an EQUILIBRIUM surface -- where the climate says the lake stands when
// nothing has happened to it. Everything that happens to it afterwards --
// rain routed down the graph, a player breaching the sill, a river diverted
// away -- is ONE int64 per basin: the signed volume delta against that baked
// equilibrium. That scalar is the authoritative water state. It persists, it
// replicates, and the drawn surface (near-field implicit fill, far-field sheet
// rects, and later the PBF particles) is derived from it.
//
// ENGINE-FREE AND INTEGER-ONLY. This IS authoritative state, so
// docs/determinism.md's float ban applies with no exception: every quantity
// here is an int64 count of ledger units or an int32 of absolute millimetres,
// and every conversion between them is exact integer arithmetic with a
// documented rounding direction. There is no float anywhere in this header and
// none may be added.
//
// -----------------------------------------------------------------------
// THE UNIT
// -----------------------------------------------------------------------
// One ledger unit is one WaterCA FILL UNIT: 1/255th of a 10 cm voxel, i.e.
// 1,000,000/255 mm^3 ~= 3921.57 mm^3 ~= 3.92 mL.
//
// That is a strange-looking number and it is the right one, because of which
// conversion has to be EXACT. Two conversions exist:
//
//   ledger <-> the water a player can move (CA cells today, despawning PBF
//       particles in Phase 3). This must be exact to the unit or the
//       conservation assertions the whole plan rests on ("emitted - despawned
//       == in-flight; ledger credits == despawn counts") become approximate,
//       and an approximate conservation check finds no bugs. Choosing the CA's
//       own currency makes this conversion the identity.
//
//   ledger <-> basin geometry (mm^3 of a hypsometry integral). This is an
//       APPROXIMATION no matter what unit is chosen: the hypsometry is
//       integrated over 1.875 m bake pixels whose ground is itself quantised,
//       so the last few mm^3 are noise several orders of magnitude below the
//       pixel. Rounding it is free.
//
// So the exact conversion is made exact and the approximate one is rounded,
// rather than the other way round. `basinUnitsFromMm3` floors, always, in both
// the volume-to-level and level-to-volume directions, which is what keeps the
// pair self-consistent (see `levelAtVolumeUnits`).
//
// -----------------------------------------------------------------------
// WHY THERE IS A CAPACITY *PROVIDER* AND NOT JUST A FUNCTION
// -----------------------------------------------------------------------
// Turning a volume into a level needs the basin's hypsometry A(h) -- the wet
// AREA as a function of stage. The bake computes it (`basins.py`) and then
// THROWS IT AWAY (`pipeline.py:4819 keep_hypsometry=False`), so a v1 tile does
// not carry it. Phase 1 of the plan ships it as basin table v2, and that work
// is in flight in another agent's tree right now.
//
// `IBasinCapacityProvider` is the seam that lets both exist. Today
// `ClientHypsometryProvider` reconstructs A(h) client-side from the basin's
// extent mask and the reconstructed ground; when v2 lands, a
// `BakedCapacityProvider` reads the shipped curve and slots into the same
// interface with no change to the ledger, the hooks, the save blob or the
// tests. Nothing above this line knows which one it is talking to.
//
// -----------------------------------------------------------------------
// WHICH GROUND -- and this one has been got wrong three times in this tree
// -----------------------------------------------------------------------
// tilestore.h:1093-1113 names the three grounds. The v0 provider uses TWO of
// them, deliberately, for two different jobs:
//
//   * THE EXTENT (which cells belong to the basin) runs on the CONTROL
//     LATTICE, `FineTileSampler::elevationMm` -- ground (1). Not because it is
//     the better surface (it is not; the prefilter stands a control point up to
//     5.6 m from the sample it interpolates) but because the BAKE measured the
//     depression component on exactly that plane, and lakes.h's whole "the
//     extent rule is shared with the bake, not re-derived" argument depends on
//     the two agreeing cell for cell. An extent that disagrees with the row it
//     ships beside is a shoreline that does not close.
//
//   * THE HYPSOMETRY (how deep each of those cells is at a given stage) runs on
//     the RECONSTRUCTED SPLINE, `reconstructedGroundMm` -- ground (2). This is
//     the surface the bake's own water depths were differenced against
//     (`water_depth_control_points`), the one `FineTile::waterMmFromDepth`
//     refuses to be handed anything else for, and the one `RiverSampler`
//     switched to at lakes.h:657-677 after the lattice was found to be up to
//     5.6 m -- 56 voxels -- off the drawn waterline. A volume-to-level curve
//     built on the lattice would mis-state the stage of a credited lake by
//     metres, which is precisely the defect this project spent 2026-08-04
//     chasing from the other end.
//
// So: lattice decides WHICH cells, spline decides HOW DEEP. Neither is ground
// (3), the amplified surface: a lake datum is flat by construction and must not
// inherit the amplifier's rills, exactly as tilestore.h says.
//
// -----------------------------------------------------------------------
// CONSERVATION
// -----------------------------------------------------------------------
// Four running totals, and one exact-integer invariant that holds after ANY
// sequence of credit()/debit() calls:
//
//     sumOfDeltas() + totalSpilled() == totalCredited() - totalDebited()
//
// `credit` is the only thing that can create a delta; `debit` is bounded below
// by the basin going EMPTY (delta == -equilibriumVolume), so no sequence of
// debits can invent water by driving a lake below its own floor; and the excess
// above the sill does not vanish, it moves to `totalSpilled()` and onto the
// spill queue, where the caller is obliged to route it into the graph and can
// be audited on having done so. Nothing here creates or destroys units.
//
// A basin the provider cannot resolve (tile not streamed in, block not
// decoded) is REFUSED, not guessed: credit() returns 0 and bumps
// `unresolvedCredits()`. Refusing loses the water, which is bad; guessing puts
// water at an invented level, which is worse and is invisible. The counter is
// what makes the refusal a number rather than an impression -- see the standing
// rule about a ran-flag distinguishable from "found nothing".

#include <algorithm>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "voxelcore/bytes.h"
#include "voxelcore/core.h"
#include "voxelcore/lakes.h"
#include "voxelcore/rivernet.h"
#include "voxelcore/tilestore.h"

namespace vxc {

// Bumped on any deliberate change to the ledger's arithmetic, its unit, or the
// persisted blob layout -- invalidates a saved ledger exactly like
// kWaterCAVersion invalidates a saved water field.
inline constexpr uint32_t kBasinLedgerVersion = 1;

// One ledger unit == one WaterCA fill unit == 1/255th of a 10 cm voxel. See
// "THE UNIT" above for why this and not litres or mm^3.
inline constexpr int64_t kBasinLedgerUnitsPerVoxel = 255;
// (100 mm)^3 == 1e6 mm^3 == one litre. The voxel volume the line above divides.
inline constexpr int64_t kVoxelVolumeMm3 =
    int64_t(kVoxelSizeMm) * int64_t(kVoxelSizeMm) * int64_t(kVoxelSizeMm);
static_assert(kVoxelVolumeMm3 == 1000000, "the unit comment above assumes a 10 cm voxel");

// Ground-elevation bin for the client-reconstructed hypsometry, in mm.
//
// ONE VOXEL, and that is the whole justification: the curve's job is to place a
// water surface on a world drawn at 10 cm, so quantising the BED to 10 cm costs
// nothing that can be seen. It is the bed that is binned, never the LEVEL --
// V(h) stays continuous and exact in h, so `levelAtVolumeUnits` still resolves
// to the millimetre. What it buys is a curve whose storage is a function of the
// basin's DEPTH RANGE (~600 bins over a 60 m basin) instead of its AREA, so the
// survey's 865 ha monster costs the same 10 KB as its 0.59 ha median.
inline constexpr int32_t kBasinHypsoBinMm = kVoxelSizeMm;

// floor(a * num / den) for a >= 0, without the intermediate a*num.
//
// Needed because the volume conversions genuinely do overflow the naive form:
// the largest surveyed basin is 865 ha, whose summed depth over the extent
// reaches ~1.5e11 mm and whose volume reaches ~5e17 mm^3 -- multiply that by
// 255 before dividing and int64 is gone. Splitting on the quotient keeps every
// intermediate under ~1e15.
constexpr int64_t mulDivFloorNonNeg(int64_t a, int64_t num, int64_t den) {
    if (a <= 0 || num <= 0 || den <= 0) return 0;
    const int64_t q = a / den, r = a % den;
    return q * num + (r * num) / den;
}

// mm^3 -> ledger units, FLOORED. See "THE UNIT": this direction is the
// approximate one on purpose, and flooring it in both directions is what keeps
// volumeAtLevel/levelAtVolume a consistent pair.
constexpr int64_t basinUnitsFromMm3(int64_t mm3) {
    return mulDivFloorNonNeg(mm3, kBasinLedgerUnitsPerVoxel, kVoxelVolumeMm3);
}

// ---------------------------------------------------------------------------
// BASIN IDENTITY
// ---------------------------------------------------------------------------
//
// v1 tiles carry u16 basin ids that are TILE-LOCAL and that the codec pins to
// 0..n-1 (`tile_codec.py:1096-1108`), so a basin's name is the pair (tile,
// local id) and nothing else. Basin table v2 replaces that with a u64 GLOBAL id
// so a lake spanning a tile edge is one basin instead of two halves and a
// dropped hole. Both have to be expressible by the same key, because the ledger
// and its save blob outlive the format change.
//
// So BasinId is one u64 with a TAG BIT:
//
//   bit 63 == 1 : v1 tile-local. bits 38-59 tileX, 16-37 tileY (both biased,
//                 22 bits each), 0-15 the local basin id. Bits 60-62 are zero.
//   bit 63 == 0 : v2 global. The low 63 bits ARE the bake's global id.
//
// Which means v2 must keep its global ids under 2^63 and must never issue 0.
// 0 is reserved here for "not a basin" -- `kNoBasin` -- so that a failed
// construction is a value a caller can test rather than a silent aliasing onto
// some real lake.
//
// The 22-bit tile fields span +/-2,097,152 tiles of 15.36 km, which is +/-32
// million km of world. A coordinate outside that yields kNoBasin rather than
// folding onto a different tile's basin, because folding is the failure that
// looks like data.
struct BasinId {
    uint64_t v = 0;

    static constexpr uint64_t kTileLocalTag = uint64_t(1) << 63;
    static constexpr int64_t kTileBias = int64_t(1) << 21;
    static constexpr uint64_t kTileMask = 0x3FFFFF; // 22 bits

    static constexpr BasinId none() { return BasinId{0}; }

    static constexpr BasinId fromTile(int32_t tileX, int32_t tileY, uint16_t localId) {
        const int64_t bx = int64_t(tileX) + kTileBias;
        const int64_t by = int64_t(tileY) + kTileBias;
        if (bx < 0 || bx > int64_t(kTileMask) || by < 0 || by > int64_t(kTileMask)) {
            return none();
        }
        return BasinId{kTileLocalTag | (uint64_t(bx) << 38) | (uint64_t(by) << 16) |
                       uint64_t(localId)};
    }

    // A basin table v2 global id. Rejects 0, which is kNoBasin.
    static constexpr BasinId fromGlobal(uint64_t globalId) {
        if (globalId == 0 || (globalId & kTileLocalTag) != 0) return none();
        return BasinId{globalId};
    }

    constexpr bool valid() const { return v != 0; }
    constexpr bool isTileLocal() const { return (v & kTileLocalTag) != 0; }
    constexpr int32_t tileX() const {
        return int32_t(int64_t((v >> 38) & kTileMask) - kTileBias);
    }
    constexpr int32_t tileY() const {
        return int32_t(int64_t((v >> 16) & kTileMask) - kTileBias);
    }
    constexpr uint16_t localId() const { return uint16_t(v & 0xFFFF); }

    friend constexpr bool operator==(BasinId, BasinId) = default;
    friend constexpr bool operator<(BasinId a, BasinId b) { return a.v < b.v; }
};

inline constexpr BasinId kNoBasin{0};

struct BasinIdHash {
    size_t operator()(BasinId id) const noexcept {
        // splitmix64 finaliser: the packed layout puts the varying bits (local
        // id) in the low 16 and the tile in the high 44, so the identity hash a
        // std::hash<uint64_t> gives would bucket every basin of one tile into a
        // handful of buckets on a power-of-two table.
        uint64_t x = id.v + 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return size_t(x ^ (x >> 31));
    }
};

// ---------------------------------------------------------------------------
// THE CAPACITY PROVIDER
// ---------------------------------------------------------------------------
//
// Everything the ledger needs to know about a basin's SHAPE. Two
// implementations are planned and the ledger cannot tell them apart:
// `ClientHypsometryProvider` below (v1 tiles: reconstruct A(h) from the extent
// mask and the spline ground) and a baked one once basin table v2 ships the
// integrated curve.
//
// Every method returns false for a basin it cannot resolve -- tile not
// streamed, block not decoded, row not present -- and a false is NEVER an
// invitation to substitute a default. See the conservation note at the top.
class IBasinCapacityProvider {
public:
    virtual ~IBasinCapacityProvider() = default;

    // Ledger units held by the basin when its surface stands at `levelMm`.
    // Monotone non-decreasing in levelMm; 0 at or below the basin floor;
    // SATURATES at the spill level, because above the sill the basin is not a
    // container any more -- the excess is the spillway's, not the lake's, and
    // the ledger routes it there rather than asking this for a number.
    virtual bool volumeAtLevelUnits(BasinId id, int32_t levelMm, int64_t& outUnits) = 0;

    // The inverse, and the exact inverse of THIS function rather than of an
    // idealised A(h): the highest millimetre level whose volume is <= `units`.
    // So volumeAtLevelUnits(levelAtVolumeUnits(u)) <= u always, with no
    // accumulating drift however many times a caller round-trips.
    virtual bool levelAtVolumeUnits(BasinId id, int64_t units, int32_t& outLevelMm) = 0;

    // The three baked reference levels, so a caller never has to carry the
    // BasinEntry alongside the id. `floorMm` is the deepest point of the pool,
    // `equilibriumMm` is the wire's surfaceMm (delta 0), `spillMm` is the sill.
    virtual bool levelsMm(BasinId id, int32_t& outFloorMm, int32_t& outEquilibriumMm,
                          int32_t& outSpillMm) = 0;

    // The baked outlet cell (`BasinEntry::outletX/Y`, "the saddle the basin
    // spills over") in world VOXEL coords -- the coordinate space RiverNode
    // uses, so a spill event can be routed into the graph with no conversion at
    // the call site. This and spillMm have been on the wire since bake_ver 8
    // and read by nothing; the spillway below is their first consumer.
    virtual bool outletVoxel(BasinId id, int64_t& outVx, int64_t& outVy) = 0;

    // Basins asked for and not resolvable. Non-zero means capacity questions
    // went unanswered, which looks exactly like "that lake is at equilibrium"
    // and must not be read as it.
    virtual uint64_t unresolvedBasins() const { return 0; }
};

// ---------------------------------------------------------------------------
// WHAT THE v0 PROVIDER NEEDS FROM THE WORLD
// ---------------------------------------------------------------------------
//
// An interface rather than a template, unlike `lakeExtentFill`, because the two
// real implementations are a streamed fine-tile sampler and a synthetic test
// fixture and both are resolved once per basin (not per cell of a hot sweep),
// so the virtual call is free and the shared code is worth more.
//
// The two elevation accessors are SEPARATE ON PURPOSE. See "WHICH GROUND"
// above: they are different surfaces, they are used for different halves of the
// computation, and an implementation that answers both from the same source has
// silently reintroduced the bug the split exists to prevent.
class IBasinTerrain {
public:
    virtual ~IBasinTerrain() = default;

    // Fine-tile pixel pitch in mm (1875 on the shipped tier).
    virtual int32_t pixelSizeMm() const = 0;

    // The baked row for this basin, or nullptr when it cannot be resolved.
    // The pointer is borrowed and need only stay valid for the duration of the
    // call that asked for it.
    virtual const BasinEntry* basinRow(BasinId id) = 0;

    // Makes the basin's whole bbox readable by the two accessors below. False
    // means some of it is not, and the caller must then build NO curve at all
    // rather than a curve with holes -- a hole reads as "very deep here", which
    // would make a lake swallow a credit and never rise.
    virtual bool prewarmBasin(BasinId id) = 0;

    // TILE-LOCAL pixel -> ground, in absolute mm. `latticeElevationMm` is the
    // control point (the bake's own plane, for the extent); `groundMm` is the
    // reconstructed spline (the drawn surface, for the depth curve).
    virtual int32_t latticeElevationMm(BasinId id, int32_t lx, int32_t ly) = 0;
    virtual int32_t groundMm(BasinId id, int32_t lx, int32_t ly) = 0;
};

// One basin's reconstructed volume-vs-stage curve.
//
// THE MODEL, in one line: V(h) = cellArea * sum over extent cells of
// max(0, h - ground_i). That is exactly "integrate A(h) dh" written as a sum
// over cells instead of a sum over levels, and it is the form that makes both
// directions O(1) instead of O(cells): binning the GROUND lets the inner sum be
// read off two prefix arrays as `cellsBelow * h - groundSumBelow`.
//
// THE EXTENT IS TAKEN AT THE SPILL LEVEL, not at the baked equilibrium. A
// basin's capacity is the pool it can hold before it overflows, so the cell set
// has to be the full pool -- the same 8-connected seeded fill lakes.h runs, with
// `spillMm` substituted for `surfaceMm`. That is a SUPERSET of the wet mask the
// renderer draws, which is the point: the difference between them is exactly
// the shoreline a filling lake is about to claim.
struct BasinHypsometry {
    int32_t floorMm = 0;        // lowest SPLINE ground inside the pool extent
    int32_t equilibriumMm = 0;  // the wire's surfaceMm
    int32_t spillMm = 0;        // the wire's spillMm (the sill)
    int64_t cellAreaMm2 = 0;    // one fine pixel
    int64_t cellCount = 0;      // cells in the pool extent
    int64_t equilibriumUnits = 0; // V(equilibriumMm) -- what "delta 0" means
    int64_t spillUnits = 0;       // V(spillMm) -- the full pool
    // cumCells[k]    = cells whose binned ground is BELOW floorMm + k*binMm
    // cumGroundMm[k] = the sum of those cells' binned ground elevations
    // Both have (bins + 1) entries, cum*[0] == 0.
    std::vector<int64_t> cumCells;
    std::vector<int64_t> cumGroundMm;

    bool valid() const { return cellCount > 0 && !cumCells.empty(); }

    // Ledger units held at `levelMm`. Saturates at spillMm (see
    // IBasinCapacityProvider::volumeAtLevelUnits).
    int64_t unitsAtLevel(int32_t levelMm) const {
        if (!valid()) return 0;
        const int64_t h = clampi64(levelMm, floorMm, spillMm);
        if (h <= floorMm) return 0;
        const int64_t bins = int64_t(cumCells.size()) - 1;
        // Cells whose BINNED bed is at or below h contribute (h - bed). Bin j
        // represents bed floorMm + j*binMm, so the last contributing bin is
        // j = (h - floorMm)/binMm and the prefix index is that PLUS ONE.
        //
        // The +1 is not an off-by-one waiting to happen, it is the difference
        // between a curve that is right and one that is systematically shallow
        // by up to a bin's worth of water per cell -- on a 2.5M-cell basin that
        // is 250 m^3 of lake quietly missing.
        const int64_t k =
            clampi64((h - int64_t(floorMm)) / kBasinHypsoBinMm + 1, 0, bins);
        const int64_t cells = cumCells[size_t(k)];
        const int64_t groundSum = cumGroundMm[size_t(k)];
        int64_t depthSumMm = cells * h - groundSum;
        if (depthSumMm <= 0) return 0;
        // Guard the one multiply that could overflow on a pathologically large
        // basin (see mulDivFloorNonNeg's note). Clamping rather than wrapping
        // makes an impossible basin read as "enormous but finite" instead of
        // negative, and a negative capacity would let the spillway route
        // backwards.
        const int64_t maxDepthSum =
            cellAreaMm2 > 0 ? (INT64_MAX / cellAreaMm2) : depthSumMm;
        depthSumMm = clampi64(depthSumMm, 0, maxDepthSum);
        return basinUnitsFromMm3(depthSumMm * cellAreaMm2);
    }

    // The highest level whose volume is <= `units`. Binary search on the
    // millimetre axis over [floorMm, spillMm] -- at most ~17 iterations for a
    // 100 m basin, each O(1).
    int32_t levelAtUnits(int64_t units) const {
        if (!valid() || units <= 0) return floorMm;
        if (units >= spillUnits) return spillMm;
        int64_t lo = floorMm, hi = spillMm;
        while (lo < hi) {
            const int64_t mid = lo + (hi - lo + 1) / 2; // upper mid: converges on the highest satisfying level
            if (unitsAtLevel(int32_t(mid)) <= units) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        return int32_t(lo);
    }
};

// The v0 capacity provider: A(h) reconstructed on the client from the extent
// mask and the spline ground, because v1 tiles do not ship it.
//
// COST, and why it is acceptable. Building one basin's curve is O(bbox) for the
// extent fill plus one 4x4 carrier stencil per extent cell for the ground --
// the survey's median basin is ~1,700 cells, so ~27k lattice reads through a
// sampler that has already decoded the blocks, once, on the first credit that
// basin ever takes. The 865 ha outlier is ~2.5M cells and will be felt; that is
// the strongest argument for basin table v2 shipping the curve, and the reason
// `hypsometryBuilds()` and `hypsometryCells()` are counters rather than
// silence.
class ClientHypsometryProvider final : public IBasinCapacityProvider {
public:
    explicit ClientHypsometryProvider(IBasinTerrain& terrain) : terrain_(&terrain) {}

    bool volumeAtLevelUnits(BasinId id, int32_t levelMm, int64_t& outUnits) override {
        const BasinHypsometry* h = curveFor(id);
        if (h == nullptr) return false;
        outUnits = h->unitsAtLevel(levelMm);
        return true;
    }

    bool levelAtVolumeUnits(BasinId id, int64_t units, int32_t& outLevelMm) override {
        const BasinHypsometry* h = curveFor(id);
        if (h == nullptr) return false;
        outLevelMm = h->levelAtUnits(units);
        return true;
    }

    bool levelsMm(BasinId id, int32_t& outFloorMm, int32_t& outEquilibriumMm,
                  int32_t& outSpillMm) override {
        const BasinHypsometry* h = curveFor(id);
        if (h == nullptr) return false;
        outFloorMm = h->floorMm;
        outEquilibriumMm = h->equilibriumMm;
        outSpillMm = h->spillMm;
        return true;
    }

    bool outletVoxel(BasinId id, int64_t& outVx, int64_t& outVy) override {
        const BasinEntry* row = terrain_->basinRow(id);
        if (row == nullptr) return false;
        const int64_t pxMm = terrain_->pixelSizeMm();
        if (pxMm <= 0) return false;
        // Tile-local pixel -> world pixel -> world voxel. The +pxMm/2 puts the
        // node at the pixel's CENTRE, matching buildFromFlowAccumulation's node
        // placement so a spill lands on the same lattice the graph is built on.
        const uint32_t tileSize = tilePixelsOf(id);
        if (tileSize == 0) return false;
        const int64_t ox = int64_t(id.tileX()) * int64_t(tileSize);
        const int64_t oy = int64_t(id.tileY()) * int64_t(tileSize);
        outVx = floorDiv((ox + int64_t(row->outletX)) * pxMm + pxMm / 2, int64_t(kVoxelSizeMm));
        outVy = floorDiv((oy + int64_t(row->outletY)) * pxMm + pxMm / 2, int64_t(kVoxelSizeMm));
        return true;
    }

    uint64_t unresolvedBasins() const override { return unresolved_; }

    // Diagnostics. `hypsometryBuilds()` is the ran-flag: zero means no curve was
    // ever built, which is a different fact from "the lakes did not move".
    uint64_t hypsometryBuilds() const { return builds_; }
    uint64_t hypsometryCells() const { return cells_; }
    size_t cachedCurveCount() const { return curves_.size(); }

    // Drops a cached curve, e.g. when the tile it was built from is evicted.
    // A curve outliving its tile is not stale, it is a lake at an arbitrary
    // depth -- the same lifetime hazard lakes.h's TileIndex re-check guards.
    void forget(BasinId id) { curves_.erase(id); }
    void forgetAll() { curves_.clear(); }

    // Exposed for tests and for the sill-faucet work in Phase 3, which wants
    // the whole curve rather than one point on it.
    const BasinHypsometry* curveFor(BasinId id) {
        // Only VALID curves are ever inserted (see the failure branch below), so
        // a hit is a hit -- no `valid()` re-test here, which would be
        // unreachable code with a plausible-sounding comment attached.
        auto it = curves_.find(id);
        if (it != curves_.end()) return &it->second;
        BasinHypsometry built;
        if (!build(id, built)) {
            // A FAILURE IS NOT CACHED, and that is the difference between a
            // lake that fills a moment late and one that never fills at all.
            // Almost every failure here is "the tile is not streamed in yet",
            // which is a fact about this instant and not about the basin --
            // caching it would mean flying toward a lake, crediting it once
            // while its tile was still loading, and having that basin refuse
            // every credit for the rest of the session.
            //
            // The retry is cheap: basinRow() and prewarmBasin() both fail in
            // O(1) on a missing tile, so a basin that genuinely does not exist
            // costs a hash lookup per attempt and shows up in
            // unresolvedBasins() as a rising count rather than a single 1.
            ++unresolved_;
            return nullptr;
        }
        auto ins = curves_.emplace(id, std::move(built));
        return &ins.first->second;
    }

    // The fine grid stride, in pixels. Required before `outletVoxel` can turn a
    // TILE-LOCAL outlet into world voxels, and there is deliberately no default:
    // guessing 8192 would put a spill event kilometres from the saddle on any
    // tile set that is not the production one, and a spill in the wrong valley
    // is worse than a spill that is refused and refunded.
    void setTilePixels(uint32_t px) { tileSizePx_ = px; }
    uint32_t tilePixels() const { return tileSizePx_; }

private:
    uint32_t tilePixelsOf(BasinId id) const {
        // v1 keys carry the tile; a v2 global key does not, and its provider
        // will answer outletVoxel from the baked row's own world coords rather
        // than through this path. Refusing here is the honest v1 answer.
        return id.isTileLocal() ? tileSizePx_ : 0u;
    }

    bool build(BasinId id, BasinHypsometry& out) {
        const BasinEntry* rowPtr = terrain_->basinRow(id);
        if (rowPtr == nullptr) return false;
        const BasinEntry row = *rowPtr; // copied: the fill below mutates a field
        const int64_t pxMm = terrain_->pixelSizeMm();
        if (pxMm <= 0) return false;
        if (row.spillMm == kNoWaterMm || row.surfaceMm == kNoWaterMm) return false;
        if (!terrain_->prewarmBasin(id)) return false;

        // THE POOL EXTENT: lakes.h's fill, run at the SILL rather than at the
        // equilibrium surface. Same function, same 8-connectivity, same seed --
        // so the cell set is the bake's own component rule and cannot drift from
        // the mask the renderer draws, it is just evaluated one level higher.
        BasinEntry atSpill = row;
        atSpill.surfaceMm = row.spillMm >= row.surfaceMm ? row.spillMm : row.surfaceMm;
        std::vector<uint8_t> mask;
        const size_t wet = lakeExtentFill(
            atSpill, [&](int32_t lx, int32_t ly) { return terrain_->latticeElevationMm(id, lx, ly); },
            mask);
        if (wet == 0) return false;

        const int32_t x0 = row.bboxX0, y0 = row.bboxY0;
        const int32_t w = int32_t(row.bboxX1) - x0 + 1;
        const int32_t h = int32_t(row.bboxY1) - y0 + 1;
        if (w <= 0 || h <= 0) return false;

        // ONE PASS over the extent, collecting the SPLINE ground of every pool
        // cell. Held in a scratch vector rather than recomputed for the second
        // pass because the second pass would be another 16 lattice reads per
        // cell, and the vector is 4 bytes per cell against the mask's 1.
        scratch_.clear();
        scratch_.reserve(wet);
        int32_t minGround = INT32_MAX, maxGround = INT32_MIN;
        for (int32_t y = 0; y < h; ++y) {
            for (int32_t x = 0; x < w; ++x) {
                if (!mask[size_t(y) * size_t(w) + size_t(x)]) continue;
                const int32_t g = terrain_->groundMm(id, x0 + x, y0 + y);
                scratch_.push_back(g);
                if (g < minGround) minGround = g;
                if (g > maxGround) maxGround = g;
            }
        }
        if (scratch_.empty()) return false;

        out.floorMm = minGround;
        out.equilibriumMm = row.surfaceMm;
        out.spillMm = atSpill.surfaceMm;
        out.cellAreaMm2 = pxMm * pxMm;
        out.cellCount = int64_t(scratch_.size());

        // A degenerate basin (sill at or below the pool floor) gets a one-bin
        // table whose capacity is exactly zero. That is the honest answer -- it
        // holds nothing and spills everything -- and it keeps every caller on
        // the same code path instead of needing a null check.
        const int64_t span = clampi64(int64_t(out.spillMm) - int64_t(out.floorMm), 0, INT64_MAX);
        const size_t bins = size_t(span / kBasinHypsoBinMm) + 1;
        std::vector<int64_t> perBinCells(bins + 1, 0);
        std::vector<int64_t> perBinGround(bins + 1, 0);
        for (int32_t g : scratch_) {
            // Cells whose bed is above the sill hold nothing below it; parking
            // them in the last bin keeps them out of every prefix sum that
            // matters without a second container to keep in step.
            const int64_t rel = clampi64(int64_t(g) - int64_t(out.floorMm), 0, span);
            const size_t b = size_t(rel / kBasinHypsoBinMm);
            const int64_t binned = int64_t(out.floorMm) + int64_t(b) * kBasinHypsoBinMm;
            perBinCells[b] += 1;
            perBinGround[b] += binned;
        }
        out.cumCells.assign(bins + 1, 0);
        out.cumGroundMm.assign(bins + 1, 0);
        for (size_t k = 0; k < bins; ++k) {
            out.cumCells[k + 1] = out.cumCells[k] + perBinCells[k];
            out.cumGroundMm[k + 1] = out.cumGroundMm[k] + perBinGround[k];
        }
        out.equilibriumUnits = out.unitsAtLevel(out.equilibriumMm);
        out.spillUnits = out.unitsAtLevel(out.spillMm);

        ++builds_;
        cells_ += uint64_t(out.cellCount);
        return true;
    }

    IBasinTerrain* terrain_;
    std::unordered_map<BasinId, BasinHypsometry, BasinIdHash> curves_;
    std::vector<int32_t> scratch_;
    uint64_t builds_ = 0;
    uint64_t cells_ = 0;
    uint64_t unresolved_ = 0;
    uint32_t tileSizePx_ = 0;
};

// ---------------------------------------------------------------------------
// THE SPILLWAY
// ---------------------------------------------------------------------------
//
// When a basin's delta would carry its surface past the sill, the excess does
// not raise the lake and it does not disappear: it becomes one of these, and
// the caller is obliged to put it somewhere (the routing graph's nearest
// segment today; a sill faucet feeding the PBF solver in Phase 3 -- see
// `routeSpills`). Until the caller does, `totalSpilled()` and `pendingSpill()`
// say exactly how much is owed.
struct BasinSpillEvent {
    BasinId basin;
    int64_t units = 0;                  // the excess, in ledger units
    int64_t outletVx = 0, outletVy = 0; // the baked saddle, in world VOXEL coords
    int32_t spillMm = 0;                // the sill's elevation, for a faucet's datum

    friend bool operator==(const BasinSpillEvent&, const BasinSpillEvent&) = default;
};

// ---------------------------------------------------------------------------
// THE LEDGER
// ---------------------------------------------------------------------------
//
// The plan calls this `FBasinLedger`; it is `vxc::BasinLedger` here because the
// F prefix is UE's and this header is engine-free. The UE subsystem owns one
// instance.
class BasinLedger {
public:
    BasinLedger() = default;
    explicit BasinLedger(IBasinCapacityProvider& capacity) : capacity_(&capacity) {}

    void setCapacityProvider(IBasinCapacityProvider* c) { capacity_ = c; }
    IBasinCapacityProvider* capacityProvider() const { return capacity_; }

    // --- the state ---------------------------------------------------------

    // Signed volume against the baked equilibrium, in ledger units. Zero for a
    // basin nothing has happened to -- which is the overwhelming majority, and
    // why this is a hash map and not a dense per-tile array.
    int64_t deltaUnits(BasinId id) const {
        const auto it = accounts_.find(id);
        return it == accounts_.end() ? 0 : it->second.delta;
    }

    // The basin's surface RIGHT NOW, in absolute mm: the baked equilibrium
    // moved by whatever the ledger holds. `fallbackMm` is returned unchanged
    // when the delta is zero (the common case, answered without touching the
    // provider) or when the provider cannot resolve the basin -- the caller
    // passes the baked `surfaceMm`, so an unresolvable basin draws exactly what
    // it drew before this system existed.
    //
    // MEMOISED per basin, keyed on nothing but "has the delta changed since we
    // last computed it", because this is called from the near-field sweep's
    // per-pixel path and a binary search there would be felt.
    int32_t levelMmFor(BasinId id, int32_t fallbackMm) {
        auto it = accounts_.find(id);
        if (it == accounts_.end() || it->second.delta == 0) return fallbackMm;
        Account& a = it->second;
        if (a.levelValid) return a.levelMm;
        int32_t level = fallbackMm;
        if (resolveLevel(id, a.delta, level)) {
            a.levelMm = level;
            a.levelValid = true;
            return level;
        }
        ++unresolvedLevels_;
        return fallbackMm;
    }

    // The same question with the delta supplied rather than looked up -- what a
    // caller asks when it wants "where WOULD this basin stand at delta d".
    // Returns false (and leaves outMm alone) when the provider cannot resolve.
    bool levelMmForDelta(BasinId id, int64_t delta, int32_t& outMm) {
        return resolveLevel(id, delta, outMm);
    }

    // Units this basin can still take before it spills, or 0 when it is already
    // at the sill. False when unresolvable.
    bool capacityToSpillUnits(BasinId id, int64_t& outUnits) {
        if (capacity_ == nullptr) return false;
        int64_t atSpill = 0, atEquilibrium = 0;
        int32_t floorMm = 0, eqMm = 0, spillMm = 0;
        if (!capacity_->levelsMm(id, floorMm, eqMm, spillMm)) return false;
        if (!capacity_->volumeAtLevelUnits(id, spillMm, atSpill)) return false;
        if (!capacity_->volumeAtLevelUnits(id, eqMm, atEquilibrium)) return false;
        const int64_t headroom = atSpill - atEquilibrium - deltaUnits(id);
        outUnits = headroom > 0 ? headroom : 0;
        return true;
    }

    // Units this basin could give up before it is EMPTY (a negative number:
    // the most negative delta it can reach). False when unresolvable.
    bool minDeltaUnits(BasinId id, int64_t& outUnits) {
        if (capacity_ == nullptr) return false;
        int64_t atEquilibrium = 0;
        int32_t floorMm = 0, eqMm = 0, spillMm = 0;
        if (!capacity_->levelsMm(id, floorMm, eqMm, spillMm)) return false;
        if (!capacity_->volumeAtLevelUnits(id, eqMm, atEquilibrium)) return false;
        outUnits = -atEquilibrium;
        return true;
    }

    // --- moving units ------------------------------------------------------

    // Adds `units` to the basin.
    //
    // RETURNS THE AMOUNT ACCEPTED INTO THE LEDGER, which is `units` when the
    // basin resolves and 0 when it does not -- never a partial. A caller
    // keeping a two-sided ledger must use the return value and never the
    // request, the same contract WaterCA::addWater and
    // RiverNetwork::withdrawToCoupler both have.
    //
    // The accepted amount is then split, by this call, between the basin's
    // delta and the spill queue: everything up to the sill raises the lake, and
    // the remainder is routed out of the basin as a BasinSpillEvent. So
    // `credit` returning 4000 with a headroom of 1000 means the lake rose by
    // 1000 and 3000 is waiting in `drainSpillEvents()`.
    int64_t credit(BasinId id, int64_t units) {
        ++creditCalls_;
        if (!id.valid() || units <= 0) return 0;
        if (capacity_ == nullptr) {
            ++unresolvedCredits_;
            return 0;
        }
        int64_t headroom = 0;
        if (!capacityToSpillUnits(id, headroom)) {
            ++unresolvedCredits_;
            return 0;
        }
        Account& a = accounts_[id];
        totalCredited_ += units;
        const int64_t intoBasin = units < headroom ? units : headroom;
        if (intoBasin > 0) {
            a.delta += intoBasin;
            a.levelValid = false;
            sumOfDeltas_ += intoBasin;
        }
        const int64_t excess = units - intoBasin;
        if (excess > 0) {
            emitSpill(id, excess);
        }
        return units;
    }

    // Removes up to `units`, bounded by the basin going empty. Returns the
    // amount ACTUALLY removed (0 for an unresolvable basin, or for one already
    // empty), which is what a caller crediting the water somewhere else must
    // move.
    int64_t debit(BasinId id, int64_t units) {
        ++debitCalls_;
        if (!id.valid() || units <= 0) return 0;
        int64_t floorDelta = 0;
        if (!minDeltaUnits(id, floorDelta)) {
            ++unresolvedDebits_;
            return 0;
        }
        const int64_t current = deltaUnits(id);
        const int64_t room = current - floorDelta; // >= 0 unless a save was hand-edited
        if (room <= 0) return 0;
        const int64_t take = units < room ? units : room;
        Account& a = accounts_[id];
        a.delta -= take;
        a.levelValid = false;
        sumOfDeltas_ -= take;
        totalDebited_ += take;
        return take;
    }

    // --- the spill queue ---------------------------------------------------

    const std::vector<BasinSpillEvent>& pendingSpill() const { return spill_; }
    int64_t pendingSpillUnits() const {
        int64_t sum = 0;
        for (const BasinSpillEvent& e : spill_) sum += e.units;
        return sum;
    }
    // Hands the queue to the caller and empties it. The caller now OWES these
    // units to something downstream; `routeSpills` below is the audited way to
    // pay, and putting them nowhere is a leak the invariant will not catch
    // (they have already left `sumOfDeltas()`), which is why totalSpilled() and
    // the router's own return value both exist.
    std::vector<BasinSpillEvent> drainSpillEvents() {
        std::vector<BasinSpillEvent> out;
        out.swap(spill_);
        return out;
    }
    // Puts units back into a basin that a downstream consumer REFUSED (no
    // segment near the outlet, a full faucet). The exact inverse of the spill
    // half of credit(), for the same reason RiverNetwork::refundFromCoupler
    // exists: a consumer that could not give units back would have to destroy
    // them or hide them.
    int64_t refundSpill(BasinId id, int64_t units) {
        if (!id.valid() || units <= 0) return 0;
        const int64_t give = units < totalSpilled_ ? units : totalSpilled_;
        if (give <= 0) return 0;
        Account& a = accounts_[id];
        a.delta += give;
        a.levelValid = false;
        sumOfDeltas_ += give;
        totalSpilled_ -= give;
        return give;
    }

    // --- conservation ledger (see the header comment) ----------------------
    int64_t sumOfDeltas() const { return sumOfDeltas_; }
    int64_t totalCredited() const { return totalCredited_; }
    int64_t totalDebited() const { return totalDebited_; }
    int64_t totalSpilled() const { return totalSpilled_; }
    // Independent re-sum over every account, for cross-checking sumOfDeltas()
    // (tests only; O(basins)).
    int64_t recomputeSumOfDeltas() const {
        int64_t sum = 0;
        for (const auto& kv : accounts_) sum += kv.second.delta;
        return sum;
    }
    bool conserves() const {
        return sumOfDeltas_ + totalSpilled_ == totalCredited_ - totalDebited_;
    }

    // --- diagnostics -------------------------------------------------------
    //
    // Every one of these is a ran-flag as much as a count: `creditCalls() == 0`
    // says the ledger was never asked, which is a different fact from "no lake
    // moved", and three absent-stat zeros produced false conclusions on this
    // project already.
    uint64_t creditCalls() const { return creditCalls_; }
    uint64_t debitCalls() const { return debitCalls_; }
    uint64_t unresolvedCredits() const { return unresolvedCredits_; }
    uint64_t unresolvedDebits() const { return unresolvedDebits_; }
    uint64_t unresolvedLevels() const { return unresolvedLevels_; }
    uint64_t spillEvents() const { return spillEvents_; }
    size_t basinCount() const { return accounts_.size(); }

    // Every basin with a non-zero delta, in ascending key order -- the
    // canonical order for the save blob, the replication payload and the
    // digest, so all three agree with each other for free.
    std::vector<std::pair<BasinId, int64_t>> snapshot() const {
        std::map<BasinId, int64_t> sorted;
        for (const auto& kv : accounts_) {
            if (kv.second.delta != 0) sorted.emplace(kv.first, kv.second.delta);
        }
        return {sorted.begin(), sorted.end()};
    }

    // Restores one basin's delta WITHOUT running it through credit/debit --
    // persistence and replication only. The ledger totals are set to match so
    // conserves() still holds after a load; what is NOT restored is the session
    // audit (creditCalls and friends), for the same reason WaterState leaves
    // WaterCA's debited_/credited_ at zero: carrying them across would make the
    // audit measure two sessions.
    void restoreDelta(BasinId id, int64_t delta) {
        if (!id.valid()) return;
        Account& a = accounts_[id];
        sumOfDeltas_ += delta - a.delta;
        a.delta = delta;
        a.levelValid = false;
    }
    // Sets the running totals to be consistent with the restored deltas.
    void restoreTotals() {
        sumOfDeltas_ = recomputeSumOfDeltas();
        totalCredited_ = sumOfDeltas_ > 0 ? sumOfDeltas_ : 0;
        totalDebited_ = sumOfDeltas_ < 0 ? -sumOfDeltas_ : 0;
        totalSpilled_ = 0;
    }

    // Drops every account and every total, KEEPING the capacity provider --
    // the provider is a binding to the world, not state about it, and a clear()
    // that silently unbound it would make the next credit refuse.
    void clear() {
        IBasinCapacityProvider* keep = capacity_;
        *this = BasinLedger();
        capacity_ = keep;
    }

    // Invalidates the cached level of every basin -- what a host calls when the
    // capacity provider's curves have been rebuilt underneath it (a tile
    // reloaded, a v2 table arriving).
    void invalidateLevels() {
        for (auto& kv : accounts_) kv.second.levelValid = false;
    }

    // Deterministic digest over every non-zero basin in key order.
    void digest(Digest& d) const {
        for (const auto& kv : snapshot()) {
            d.u64(kv.first.v);
            d.i64(kv.second);
        }
    }

private:
    struct Account {
        int64_t delta = 0;
        int32_t levelMm = 0;
        bool levelValid = false;
    };

    bool resolveLevel(BasinId id, int64_t delta, int32_t& outMm) {
        if (capacity_ == nullptr) return false;
        int32_t floorMm = 0, eqMm = 0, spillMm = 0;
        if (!capacity_->levelsMm(id, floorMm, eqMm, spillMm)) return false;
        int64_t atEquilibrium = 0;
        if (!capacity_->volumeAtLevelUnits(id, eqMm, atEquilibrium)) return false;
        const int64_t target = atEquilibrium + delta;
        return capacity_->levelAtVolumeUnits(id, target > 0 ? target : 0, outMm);
    }

    void emitSpill(BasinId id, int64_t units) {
        BasinSpillEvent e;
        e.basin = id;
        e.units = units;
        int32_t floorMm = 0, eqMm = 0, spillMm = 0;
        if (capacity_ != nullptr && capacity_->levelsMm(id, floorMm, eqMm, spillMm)) {
            e.spillMm = spillMm;
        }
        if (capacity_ == nullptr || !capacity_->outletVoxel(id, e.outletVx, e.outletVy)) {
            // A basin with no resolvable outlet still spills -- the units leave
            // it either way, and pretending otherwise would make a terminal
            // lake grow without bound. The event carries (0,0) and the router
            // will fail to place it, which refunds it. That round trip is the
            // designed behaviour, not a hole: it back-pressures the lake exactly
            // as a blocked outfall should.
            e.outletVx = 0;
            e.outletVy = 0;
        }
        totalSpilled_ += units;
        ++spillEvents_;
        spill_.push_back(e);
    }

    IBasinCapacityProvider* capacity_ = nullptr;
    std::unordered_map<BasinId, Account, BasinIdHash> accounts_;
    std::vector<BasinSpillEvent> spill_;

    int64_t sumOfDeltas_ = 0;
    int64_t totalCredited_ = 0;
    int64_t totalDebited_ = 0;
    int64_t totalSpilled_ = 0;

    uint64_t creditCalls_ = 0;
    uint64_t debitCalls_ = 0;
    uint64_t unresolvedCredits_ = 0;
    uint64_t unresolvedDebits_ = 0;
    uint64_t unresolvedLevels_ = 0;
    uint64_t spillEvents_ = 0;
};

// Drains the spill queue through `inject`, which must return how many of the
// units it ACCEPTED (the RiverNetwork/coupler contract). Anything refused is
// refunded to the basin it came from, so the pass is exact on both sides and a
// downstream that cannot take the water back-pressures the lake instead of
// losing it.
//
// `inject(vx, vy, units, spillMm)` -> accepted. Templated rather than an
// interface so this header stays free of rivernet.h: the graph is one consumer
// of a spill and the Phase 3 sill faucet will be another, and neither belongs
// in the ledger's dependency set.
//
// Returns the total accepted. `outRefunded` (optional) reports what came back,
// which is the number that must be zero on a healthy frame and is otherwise the
// whole story.
template <class InjectFn>
int64_t routeSpills(BasinLedger& ledger, InjectFn&& inject, int64_t* outRefunded = nullptr) {
    int64_t accepted = 0, refunded = 0;
    for (const BasinSpillEvent& e : ledger.drainSpillEvents()) {
        const int64_t took =
            clampi64(inject(e.outletVx, e.outletVy, e.units, e.spillMm), 0, e.units);
        accepted += took;
        const int64_t back = e.units - took;
        if (back > 0) refunded += ledger.refundSpill(e.basin, back);
    }
    if (outRefunded != nullptr) *outRefunded = refunded;
    return accepted;
}

// ---------------------------------------------------------------------------
// PERSISTENCE
// ---------------------------------------------------------------------------
//
// Little-endian, integer only, same primitives every other wire format here
// uses (bytes.h). Deterministic by construction: `snapshot()` is key-sorted, so
// a re-serialised load is byte-identical and there is nothing to sort at save
// time.
//
//   u32 magic, u32 kBasinLedgerVersion
//   u64 count, then per basin: u64 basinId, i64 delta
//   i64 sumOfDeltas   (integrity cross-check, exactly WaterState's totalVolume
//                      trick: a blob whose rows do not add up to their own
//                      header is refused rather than half-applied)
//
// NOT versioned against the BAKE. A delta is a volume, and a volume is
// meaningful against any capacity curve -- so a ledger saved before basin table
// v2 and loaded after it puts the same water in the same lake at whatever level
// the better curve says, which is the correct behaviour and the reason the
// authority is a VOLUME and not a level.
struct BasinLedgerState {
    static constexpr uint32_t kMagic = 0x4C424E42; // "BNBL" little-endian

    // Appends; does not clear. Mirrors WaterState::serialize.
    static void serialize(const BasinLedger& ledger, std::vector<uint8_t>& out) {
        ByteWriter w(out);
        w.u32(kMagic);
        w.u32(kBasinLedgerVersion);
        const std::vector<std::pair<BasinId, int64_t>> rows = ledger.snapshot();
        w.u64(uint64_t(rows.size()));
        int64_t sum = 0;
        for (const auto& kv : rows) {
            w.u64(kv.first.v);
            w.u64(uint64_t(kv.second));
            sum += kv.second;
        }
        w.u64(uint64_t(sum));
    }

    // Decodes and validates without touching `ledger`, then applies. False --
    // with the ledger untouched -- on bad magic, a version mismatch,
    // truncation, trailing bytes, a zero/duplicate/non-ascending key, or a
    // cross-check mismatch. Every one of those is a blob that would otherwise
    // put water at the wrong level in a world the player keeps.
    static bool load(const uint8_t* data, size_t size, BasinLedger& ledger) {
        ByteReader r(data, size);
        uint32_t magic = 0, version = 0;
        uint64_t count = 0;
        if (!r.u32(magic) || magic != kMagic) return false;
        if (!r.u32(version) || version != kBasinLedgerVersion) return false;
        if (!r.u64(count)) return false;
        // Bound the allocation by the bytes that must exist to back it, so a
        // garbage length header fails fast instead of asking for 2^64 rows.
        if (count > (size / 16) + 1) return false;
        std::vector<std::pair<BasinId, int64_t>> rows;
        rows.reserve(size_t(count));
        int64_t sum = 0;
        uint64_t prevKey = 0;
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t key = 0, raw = 0;
            if (!r.u64(key) || !r.u64(raw)) return false;
            if (key == 0) return false;                        // kNoBasin is not a basin
            if (i > 0 && key <= prevKey) return false;         // must be strictly ascending
            prevKey = key;
            const int64_t delta = int64_t(raw);
            rows.emplace_back(BasinId{key}, delta);
            sum += delta;
        }
        uint64_t storedSum = 0;
        if (!r.u64(storedSum)) return false;
        if (int64_t(storedSum) != sum) return false;
        if (!r.atEnd()) return false;

        for (const auto& kv : rows) ledger.restoreDelta(kv.first, kv.second);
        ledger.restoreTotals();
        return true;
    }
};

// ---------------------------------------------------------------------------
// ADAPTERS OVER THE SHIPPED FINE TIER
// ---------------------------------------------------------------------------
//
// Everything above this line is world-free: it talks to interfaces and a test
// fixture satisfies them in a few dozen lines. Below it are the three concrete
// implementations that read real baked tiles, collected here rather than
// scattered so that "what does Phase 2 need from a tile" is one section of one
// file.
//
// `FineTileBakedWaterSource` lives here rather than in rivernet.h on purpose:
// rivernet.h's documented dependency set is tiles.h and nothing else (it is
// terrain-free the same way waterca.h is), and putting the fine-tile decoder
// behind every include of the routing graph would end that. See
// IBakedWaterSource's own comment.

// `IBasinDatumSource` (lakes.h) over a `BasinLedger`. This is the object that
// makes a credited basin visibly rise: hand it to `LakeSampler::setDatumSource`
// and the near-field implicit fill, the sheet gather and the debug marker all
// start reading the ledger's level instead of the wire's.
class BasinLedgerDatumSource final : public IBasinDatumSource {
public:
    explicit BasinLedgerDatumSource(BasinLedger& ledger) : ledger_(&ledger) {}

    int32_t basinDatumMm(int32_t tx, int32_t ty, const BasinEntry& baked) override {
        const BasinId id = BasinId::fromTile(tx, ty, baked.basinId);
        if (!id.valid()) return baked.surfaceMm;
        return ledger_->levelMmFor(id, baked.surfaceMm);
    }

private:
    BasinLedger* ledger_;
};

// `IBasinTerrain` over a streamed `FineTileSampler`.
//
// THREADING mirrors FineTileSampler's and LakeSampler's exactly: the first
// query inside a basin decodes blocks, so queries MUTATE. Build curves from one
// thread (`prewarmBasin` then the credit that triggers the build); after that
// the ledger's level lookups are pure reads of a cached curve.
class FineTileBasinTerrain final : public IBasinTerrain {
public:
    explicit FineTileBasinTerrain(FineTileSampler& tiles) : tiles_(&tiles) {}

    int32_t pixelSizeMm() const override { return tiles_->pixelSizeMm(); }

    const BasinEntry* basinRow(BasinId id) override {
        // A v2 GLOBAL id has no tile in it by construction, and this adapter
        // reads a v1 per-tile table. Refusing is the honest answer; the v2
        // provider will resolve those from the merged table when it lands.
        if (!id.isTileLocal()) return nullptr;
        const FineTile* t = tiles_->findTile(id.tileX(), id.tileY());
        if (t == nullptr || !t->hasBasins() || !t->basinsResident()) return nullptr;
        const std::vector<BasinEntry>& rows = t->basins();
        const size_t local = size_t(id.localId());
        if (local >= rows.size()) return nullptr;
        return &rows[local];
    }

    bool prewarmBasin(BasinId id) override {
        const BasinEntry* b = basinRow(id);
        if (b == nullptr) return false;
        const uint32_t size = tiles_->tileSize();
        if (size == 0) return false;
        const int64_t ox = int64_t(id.tileX()) * int64_t(size);
        const int64_t oy = int64_t(id.tileY()) * int64_t(size);
        // ONE PIXEL OF APRON, because `reconstructedGroundMm` gathers a 4x4
        // stencil spanning px-1..px+2. Prewarming the bare bbox leaves the rim
        // block-faulting inside the fill -- or worse, reading a missing tile as
        // elevation 0, which is a 600 m cliff at the shoreline.
        if (tiles_->prewarm(ox + b->bboxX0 - 1, oy + b->bboxY0 - 1, ox + b->bboxX1 + 2,
                            oy + b->bboxY1 + 2)) {
            return true;
        }
        // The apron can cross into a NEIGHBOURING tile that is not resident,
        // which is a much smaller problem than the basin itself being absent:
        // it costs the stencil accuracy of a one-pixel rim. Fall back to the
        // bare bbox and COUNT it, rather than refusing a whole lake over its
        // outermost row of cells.
        if (tiles_->prewarm(ox + b->bboxX0, oy + b->bboxY0, ox + b->bboxX1, oy + b->bboxY1)) {
            ++apronMisses_;
            return true;
        }
        return false;
    }

    int32_t latticeElevationMm(BasinId id, int32_t lx, int32_t ly) override {
        int64_t ox = 0, oy = 0;
        if (!originOf(id, ox, oy)) return 0;
        return tiles_->elevationMm(ox + lx, oy + ly);
    }

    int32_t groundMm(BasinId id, int32_t lx, int32_t ly) override {
        int64_t ox = 0, oy = 0;
        if (!originOf(id, ox, oy)) return 0;
        // THE SPLINE, not the lattice -- see this header's "WHICH GROUND".
        return reconstructedGroundMm(*tiles_, ox + lx, oy + ly);
    }

    // Basins whose stencil apron could not be prewarmed. Non-zero means some
    // shoreline cells were integrated against a 4x4 gather that reached a
    // non-resident block, which the sampler answers as 0 mm -- a fact worth a
    // number rather than a shrug.
    uint64_t apronMisses() const { return apronMisses_; }

private:
    bool originOf(BasinId id, int64_t& ox, int64_t& oy) const {
        const uint32_t size = tiles_->tileSize();
        if (size == 0 || !id.isTileLocal()) return false;
        ox = int64_t(id.tileX()) * int64_t(size);
        oy = int64_t(id.tileY()) * int64_t(size);
        return true;
    }

    FineTileSampler* tiles_;
    uint64_t apronMisses_ = 0;
};

// `IBakedWaterSource` (rivernet.h) over a streamed `FineTileSampler`: the water
// plane and the flow plane, per fine pixel, for `buildFromBakedWater`.
//
// WHY IT CACHES DECODED BLOCKS RATHER THAN GOING THROUGH RiverSampler. The
// builder scans a whole rectangle pixel by pixel and needs the WET BIT, which
// `RiverSampler::surfaceAtPixel` deliberately does not expose (a banded dry
// cell and a wet cell both return a finite surface there, which is right for a
// waterline and wrong for a channel test). It also needs the flow plane, which
// no existing sampler reads at all. So this holds its own two block caches --
// over the SAME FineTileSampler, so it cannot see a different tile set than the
// lakes and ribbons do.
class FineTileBakedWaterSource final : public IBakedWaterSource {
public:
    explicit FineTileBakedWaterSource(FineTileSampler& tiles) : tiles_(&tiles) {}

    int32_t pixelSizeMm() const override { return tiles_->pixelSizeMm(); }

    bool waterAt(int64_t px, int64_t py, int32_t& outSurfaceMm, bool& outWet) override {
        const FineTile* t = nullptr;
        uint32_t lx = 0, ly = 0;
        if (!locate(px, py, t, lx, ly) || !t->hasWater()) return false;
        const std::vector<int16_t>* block = waterBlock(*t, px, py, lx, ly);
        if (block == nullptr) return false;
        const uint32_t dim = t->blockDim();
        const int16_t cp = (*block)[size_t(ly & (dim - 1)) * dim + size_t(lx & (dim - 1))];
        outWet = FineTile::waterCpIsWet(cp);
        // The SPLINE ground, through the one function that is allowed to be a
        // `waterMmFromDepth` argument. Only paid for cells that are not the dry
        // sentinel, which is RiverSampler's own early-out and the reason ~99%
        // of a tile costs no stencil at all.
        if (cp == kWaterDryDepth || cp == kWaterNoLevel) {
            outSurfaceMm = kNoWaterMm;
            outWet = false;
            return true; // RESOLVED and dry -- a different fact from unresolvable
        }
        outSurfaceMm = FineTile::waterMmFromDepth(cp, reconstructedGroundMm(*tiles_, px, py));
        return true;
    }

    bool flowAt(int64_t px, int64_t py, uint8_t& outFlow) override {
        const FineTile* t = nullptr;
        uint32_t lx = 0, ly = 0;
        if (!locate(px, py, t, lx, ly) || !t->hasFlow()) return false;
        const std::vector<uint8_t>* block = flowBlock(*t, px, py, lx, ly);
        if (block == nullptr) return false;
        const uint32_t dim = t->blockDim();
        outFlow = (*block)[size_t(ly & (dim - 1)) * dim + size_t(lx & (dim - 1))];
        return true;
    }

    size_t residentWaterBlocks() const { return water_.size(); }
    size_t residentFlowBlocks() const { return flow_.size(); }
    // Blocks whose bytes were there and did not decode. Non-zero means the
    // graph is short reaches for a reason that is NOT "no river here".
    uint64_t unresolvedBlocks() const { return unresolved_; }

    void clearCaches() {
        water_.clear();
        flow_.clear();
    }

private:
    bool locate(int64_t px, int64_t py, const FineTile*& outTile, uint32_t& outLx,
                uint32_t& outLy) const {
        const uint32_t size = tiles_->tileSize();
        if (size == 0) return false;
        const int32_t tx = int32_t(floorDiv(px, size)), ty = int32_t(floorDiv(py, size));
        const FineTile* t = tiles_->findTile(tx, ty);
        if (t == nullptr) return false;
        outTile = t;
        outLx = uint32_t(px - int64_t(tx) * size);
        outLy = uint32_t(py - int64_t(ty) * size);
        return true;
    }

    static uint64_t blockKey(int64_t px, int64_t py, uint32_t log2) {
        // The BLOCK's world position, so the key is unique across tiles with no
        // tile coordinate in it -- one shift instead of the four-way xor a
        // (tx,ty,bx,by) pack would need, and no aliasing to reason about.
        const uint64_t bx = uint64_t(px >> log2) & 0xFFFFFFFFull;
        const uint64_t by = uint64_t(py >> log2) & 0xFFFFFFFFull;
        return (bx << 32) | by;
    }

    const std::vector<int16_t>* waterBlock(const FineTile& t, int64_t px, int64_t py, uint32_t lx,
                                           uint32_t ly) {
        const uint32_t log2 = t.blockLog2();
        const uint64_t k = blockKey(px, py, log2);
        auto it = water_.find(k);
        if (it != water_.end()) return &it->second;
        std::vector<int16_t> decoded;
        if (!t.decodeWaterBlock(lx >> log2, ly >> log2, decoded)) {
            ++unresolved_;
            return nullptr;
        }
        return &water_.emplace(k, std::move(decoded)).first->second;
    }

    const std::vector<uint8_t>* flowBlock(const FineTile& t, int64_t px, int64_t py, uint32_t lx,
                                          uint32_t ly) {
        const uint32_t log2 = t.blockLog2();
        const uint64_t k = blockKey(px, py, log2);
        auto it = flow_.find(k);
        if (it != flow_.end()) return &it->second;
        std::vector<uint8_t> decoded;
        if (!t.decodeFlowBlock(lx >> log2, ly >> log2, decoded)) {
            ++unresolved_;
            return nullptr;
        }
        return &flow_.emplace(k, std::move(decoded)).first->second;
    }

    FineTileSampler* tiles_;
    std::unordered_map<uint64_t, std::vector<int16_t>> water_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> flow_;
    uint64_t unresolved_ = 0;
};

} // namespace vxc
