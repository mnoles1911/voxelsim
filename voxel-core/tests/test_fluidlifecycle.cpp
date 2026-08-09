// The faucet/sink lifecycle's pure logic (voxelcore/fluidlifecycle.h) --
// Phase 3 integration of docs/water-rearchitecture-plan-2026-08-09.md.
//
// The one test in this file that MUST exist is the unit-conversion chain: a
// factor-of-255 slip between particles and ledger units is the classic error
// of this design (basinledger.h "THE UNIT"), it is invisible in any screenshot
// (the lake just fills 255x too fast or too slow), and only an end-to-end
// derivation check catches it before the owner does.

#include "voxelcore/fluidlifecycle.h"
#include "vxctest.h"

using namespace vxc;

namespace {

VXC_TEST(fluid_unit_conversion_chain) {
    // The derivation, end to end: one particle == one 10 cm voxel of water.
    // A 10 cm voxel is (100 mm)^3 = 1e6 mm^3 = exactly one litre.
    CHECK_EQ(kVoxelSizeMm, 100);
    CHECK_EQ(kVoxelVolumeMm3, int64_t(1'000'000));
    // One ledger unit is 1/255 voxel (basinledger.h), so one particle == 255
    // ledger units -- and the constant must be DEFINED from the ledger's, not
    // restated.
    CHECK_EQ(kFluidLedgerUnitsPerParticle, int64_t(255));
    CHECK_EQ(kFluidLedgerUnitsPerParticle, kBasinLedgerUnitsPerVoxel);
    CHECK_EQ(kFluidLitresPerParticle, int64_t(1));

    // The two failure modes this test exists for, stated as inequalities so a
    // future "simplification" to either wrong constant fails loudly:
    CHECK(kFluidLedgerUnitsPerParticle != 1);            // "1 unit per particle"
    CHECK(fluidParticleUnits(1) != kVoxelVolumeMm3);     // "1 mm^3 unit" confusion

    // Round trips.
    CHECK_EQ(fluidParticleUnits(0), int64_t(0));
    CHECK_EQ(fluidParticleUnits(1), int64_t(255));
    CHECK_EQ(fluidParticleUnits(4), int64_t(1020));
    CHECK_EQ(fluidWholeParticlesFromUnits(1020), int64_t(4));
    CHECK_EQ(fluidWholeParticlesFromUnits(254), int64_t(0));  // floor, never round up
    CHECK_EQ(fluidWholeParticlesFromUnits(255), int64_t(1));
    CHECK_EQ(fluidWholeParticlesFromUnits(509), int64_t(1));
    CHECK_EQ(fluidWholeParticlesFromUnits(-255), int64_t(0)); // negative units are not particles
    // Cross-check against basinledger's own mm^3 conversion: 255 units is one
    // voxel is 1e6 mm^3, through basinUnitsFromMm3's floor.
    CHECK_EQ(basinUnitsFromMm3(kVoxelVolumeMm3), kFluidLedgerUnitsPerParticle);
}

VXC_TEST(fluid_faucet_rate_derivation) {
    // The plan's worked example: 8e6 m^3/yr ~= 253/s.
    //   8e6 m^3/yr * 1000 L/m^3 / 31,557,600 s = 253.506... particles/s
    CHECK_EQ(kFluidSecondsPerYear, int64_t(31'557'600));
    FluidFaucetAccumulator acc;
    const int64_t q = 8'000'000;
    // One second, in one add.
    const int64_t oneSecond = acc.addMicros(q, 1'000'000);
    CHECK_EQ(oneSecond, int64_t(253));  // floor(253.506)
    // The fractional remainder is carried, not lost: after ~2 more seconds the
    // running floor has advanced by the carried fraction.
    const int64_t twoMore = acc.addMicros(q, 2'000'000);
    // total after 3 s = floor(3 * 253.506...) = 760 -> 507 owed now.
    CHECK_EQ(oneSecond + twoMore, int64_t(760));
}

VXC_TEST(fluid_faucet_accumulator_no_drift) {
    // 60 Hz for one simulated hour: total emitted must equal the closed-form
    // floor exactly -- the integer accumulator's whole reason to exist.
    FluidFaucetAccumulator acc;
    const int64_t q = 8'000'000;
    const int64_t dtMicros = 16'667;  // one 60 Hz frame, not evenly dividing 1e6 on purpose
    const int64_t steps = 3600LL * 60LL;
    int64_t emitted = 0;
    for (int64_t i = 0; i < steps; ++i) emitted += acc.addMicros(q, dtMicros);
    const int64_t totalCarry = q * dtMicros * steps;
    CHECK_EQ(emitted, totalCarry / kFluidFaucetCarryPerParticle);
    // And the carry holds exactly the remainder.
    CHECK_EQ(acc.carry, totalCarry % kFluidFaucetCarryPerParticle);
}

VXC_TEST(fluid_faucet_accumulator_edges) {
    FluidFaucetAccumulator acc;
    CHECK_EQ(acc.addMicros(0, 1'000'000), int64_t(0));
    CHECK_EQ(acc.addMicros(-5, 1'000'000), int64_t(0));
    CHECK_EQ(acc.addMicros(1000, 0), int64_t(0));
    CHECK_EQ(acc.carry, int64_t(0));
    // A tiny stream still eventually emits: 1 m^3/yr = 1000 L/yr.
    // One particle every kFluidSecondsPerYear/1000 s = ~8.77 h.
    int64_t emitted = 0;
    for (int i = 0; i < 32'000; ++i) emitted += acc.addMicros(1, 1'000'000'000);  // 1000 s steps
    // 32e6 s at 1000 L / 31,557,600 s = 1013.9... -> 1013 whole particles.
    CHECK_EQ(emitted, (int64_t(1) * 32'000'000'000'000LL) / kFluidFaucetCarryPerParticle);
    CHECK(emitted > 0);
}

VXC_TEST(fluid_basin_pick) {
    // Region: [0, 10000] x [0, 10000] mm (a 10 m box), centre (5000, 5000).
    const int64_t r0 = 0, r1 = 10'000;
    FluidBasinCandidate c[4];
    // 0: overlaps, holds water, centre far (offset box).
    c[0] = {8'000, 8'000, 30'000, 30'000, true};
    // 1: overlaps, holds water, centre nearest.
    c[1] = {-2'000, -2'000, 12'000, 12'000, true};
    // 2: overlaps, nearest of all, but DRY -- must be skipped.
    c[2] = {4'000, 4'000, 6'000, 6'000, false};
    // 3: holds water but no overlap.
    c[3] = {20'000, 20'000, 40'000, 40'000, true};
    CHECK_EQ(fluidPickBasinSink(c, 4, r0, r0, r1, r1), int32_t(1));
    // No candidates -> -1, and a caller must treat that as "no sink", not 0.
    CHECK_EQ(fluidPickBasinSink(c, 0, r0, r0, r1, r1), int32_t(-1));
    // All dry -> -1.
    FluidBasinCandidate dry[1] = {{0, 0, 10, 10, false}};
    CHECK_EQ(fluidPickBasinSink(dry, 1, r0, r0, r1, r1), int32_t(-1));
    // Touching edges count as intersecting (interval overlap is inclusive):
    FluidBasinCandidate touch[1] = {{10'000, 0, 20'000, 10'000, true}};
    CHECK_EQ(fluidPickBasinSink(touch, 1, r0, r0, r1, r1), int32_t(0));
    // Determinism on a tie: two identical candidates -> the lower index.
    FluidBasinCandidate tie[2] = {{0, 0, 10'000, 10'000, true}, {0, 0, 10'000, 10'000, true}};
    CHECK_EQ(fluidPickBasinSink(tie, 2, r0, r0, r1, r1), int32_t(0));
}

}  // namespace
