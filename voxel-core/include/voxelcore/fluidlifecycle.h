#pragma once
// The faucet/sink lifecycle's PURE logic (water re-architecture Phase 3
// integration, docs/water-rearchitecture-plan-2026-08-09.md §4 "macro-
// hydrology" and the Phase 3 roadmap row): the unit conversion between PBF
// particles and the scalar hydrology ledger, the faucet-rate accumulator that
// turns a baked discharge into an exact particle emission schedule, and the
// basin-sink pick. Everything here is engine-free and unit-tested; the UE
// subsystem (VoxelFluidSubsystem.cpp) calls these and adds no arithmetic of
// its own, because every number in this file is one a factor-of-255 or a
// seconds-per-year slip would silently corrupt.
//
// -----------------------------------------------------------------------
// THE UNIT CONVERSION, derived once, in one place
// -----------------------------------------------------------------------
// The particle contract (VoxelFluidContract.ush:27-29) fixes the particle:
//
//     one particle == one 10 cm voxel of water at rest spacing.
//
// One 10 cm voxel is (100 mm)^3 = 1,000,000 mm^3 = exactly ONE LITRE
// (kVoxelVolumeMm3, basinledger.h). The scalar hydrology ledger's unit
// (basinledger.h "THE UNIT") is one WaterCA fill unit = 1/255th of that same
// voxel. Therefore:
//
//     1 particle == 1 L == 1 voxel == 255 ledger units.
//
// The classic error here is crediting 1 unit per particle (255x too little
// water in the lake) or 255 litres per particle (255x too much). The constant
// below is defined FROM kBasinLedgerUnitsPerVoxel rather than as a literal so
// the two cannot drift, and test_fluidlifecycle.cpp asserts the whole chain
// (particle -> litre -> mm^3 -> units) end to end.

#include <cstdint>

#include "voxelcore/basinledger.h"
#include "voxelcore/core.h"

namespace vxc {

// One particle is one voxel of water; the ledger counts 1/255 voxels.
// == 255. See the derivation above; do not replace with a literal.
inline constexpr int64_t kFluidLedgerUnitsPerParticle = kBasinLedgerUnitsPerVoxel;

// One particle is exactly one litre (the contract's own line: "one particle
// == one voxel of water == 255 fill units == 1 L"). Kept as a named constant
// because the faucet-rate derivation below multiplies m^3 by it.
inline constexpr int64_t kFluidLitresPerParticle = 1;
static_assert(kVoxelVolumeMm3 == 1'000'000,
              "one 10 cm voxel must be exactly one litre or the particle<->ledger "
              "conversion above is wrong at its root");
static_assert(kFluidLedgerUnitsPerParticle == 255,
              "the particle<->ledger conversion is 255 units per particle by "
              "derivation; if kBasinLedgerUnitsPerVoxel moved, every faucet and "
              "sink must be re-audited");

// Ledger units a batch of despawned particles credits. The EXACT direction
// (basinledger.h "THE UNIT": ledger <-> particles must be the identity, and it
// is -- an integer multiply).
constexpr int64_t fluidParticleUnits(int64_t particles) {
    return particles * kFluidLedgerUnitsPerParticle;
}

// Whole particles representable by a pile of ledger units, FLOORED -- the
// remainder stays in ledger space (refunded or carried), never rounded into a
// phantom particle.
constexpr int64_t fluidWholeParticlesFromUnits(int64_t units) {
    return units >= 0 ? units / kFluidLedgerUnitsPerParticle : 0;
}

// ---------------------------------------------------------------------------
// FAUCET RATE: Q m^3/yr -> particles/s, as an exact integer accumulator
// ---------------------------------------------------------------------------
//
// The bake ships headwater discharge as u32 m^3/yr (tilestore.h HeadEntry::
// qM3PerYear). The derivation:
//
//     particles/s = Q [m^3/yr] * 1000 [L/m^3] / kFluidSecondsPerYear [s/yr]
//
// (1 particle = 1 L). With the Julian year, 8e6 m^3/yr = 8e9 L / 31,557,600 s
// = 253.5 particles/s -- the plan's own worked example.
//
// The accumulator is integer-exact: it carries Q * dt in (m^3 * us / yr) and
// one particle costs kFluidSecondsPerYear * 1000 * 1000000 / 1000 of that --
// i.e. seconds-per-year * 1000, once the L/m^3 and s/us factors cancel:
//
//     particles = carry [m^3*us/yr] * 1000 [L/m^3] / (secPerYear * 1e6 [us/s])
//               = carry / (secPerYear * 1000)
//
// so no float ever touches the schedule and a long-running faucet emits
// exactly floor(rate * elapsed) particles with no drift, which is what lets
// the conservation line reconcile emissions against Q to the particle.

// The Julian year, the convention hydrology uses for m^3/yr (365.25 d).
inline constexpr int64_t kFluidSecondsPerYear = 31'557'600;

// Accumulator ticks per particle: see the derivation above.
inline constexpr int64_t kFluidFaucetCarryPerParticle = kFluidSecondsPerYear * 1000;

struct FluidFaucetAccumulator {
    int64_t carry = 0;

    // Adds dtMicros of wall time at qM3PerYear and returns the whole particles
    // now owed. Overflow-safe by range: Q is a u32 (<= 4.3e9) and dtMicros for
    // one frame is < 1e6, so the product is < 4.3e15 << INT64_MAX; the carry
    // never exceeds the product plus one particle's cost.
    int64_t addMicros(int64_t qM3PerYear, int64_t dtMicros) {
        if (qM3PerYear <= 0 || dtMicros <= 0) return 0;
        carry += qM3PerYear * dtMicros;
        const int64_t particles = carry / kFluidFaucetCarryPerParticle;
        carry -= particles * kFluidFaucetCarryPerParticle;
        return particles;
    }

    // Returns particles addMicros paid out but the host could not emit this
    // tick (per-tick anti-burst cap, emission budget) back to the schedule:
    // the exact inverse of the payout, so a clamped faucet streams its
    // backlog out over later ticks instead of dumping it -- and instead of
    // dropping it. The HOST bounds what it carries back (an unbounded
    // carry-back against a rate cap is unpayable debt; the subsystem routes
    // anything beyond ~1 s of rate through the scalar graph instead).
    void carryBackParticles(int64_t particles) {
        if (particles > 0) carry += particles * kFluidFaucetCarryPerParticle;
    }

    void reset() { carry = 0; }
};

// ---------------------------------------------------------------------------
// BASIN SINK PICK (v1: one basin)
// ---------------------------------------------------------------------------
//
// v1 uploads ONE basin's clipped bbox + datum to the finalize kernel (the
// documented limit: a region straddling two lakes despawns into only the
// picked one; the other lake's particles pool on its surface until v2 uploads
// a table). The pick: among water-holding candidates whose bbox intersects
// the active region, the one whose bbox centre is nearest the region centre.
// Integer mm so the choice is deterministic; ties break to the lower index,
// which is stable because callers pass candidates in registry order.

struct FluidBasinCandidate {
    int64_t minXMm = 0, minYMm = 0, maxXMm = 0, maxYMm = 0;  // world bbox
    bool holdsWater = false;
};

// Returns the picked candidate's index, or -1 when nothing qualifies.
// The region rectangle is inclusive-exclusive agnostic: pure interval overlap.
inline int32_t fluidPickBasinSink(const FluidBasinCandidate* candidates, int32_t count,
                                  int64_t regionMinXMm, int64_t regionMinYMm,
                                  int64_t regionMaxXMm, int64_t regionMaxYMm) {
    const int64_t rcx = regionMinXMm + (regionMaxXMm - regionMinXMm) / 2;
    const int64_t rcy = regionMinYMm + (regionMaxYMm - regionMinYMm) / 2;
    int32_t best = -1;
    int64_t bestD2 = INT64_MAX;
    for (int32_t i = 0; i < count; ++i) {
        const FluidBasinCandidate& c = candidates[i];
        if (!c.holdsWater) continue;
        if (c.maxXMm < regionMinXMm || c.minXMm > regionMaxXMm || c.maxYMm < regionMinYMm ||
            c.minYMm > regionMaxYMm) {
            continue;  // no overlap with the active region
        }
        const int64_t cx = c.minXMm + (c.maxXMm - c.minXMm) / 2;
        const int64_t cy = c.minYMm + (c.maxYMm - c.minYMm) / 2;
        const int64_t dx = cx - rcx, dy = cy - rcy;
        // Basin bboxes and the active region are km-scale at most, so dx/dy fit
        // comfortably in 32 bits of mm... except they do not (a 15.36 km tile is
        // 1.5e7 mm), so square in int64: 1.5e7^2 = 2.4e14, fine.
        const int64_t d2 = dx * dx + dy * dy;
        if (d2 < bestD2) {
            bestD2 = d2;
            best = i;
        }
    }
    return best;
}

}  // namespace vxc
