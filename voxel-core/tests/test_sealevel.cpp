// SEA LEVEL, one symbol (voxelcore/core.h kSeaLevelMm).
//
// The datum was twenty-one unnamed literal zeros across five languages, plus
// three named constants that did not agree on units and one -- rivernet.h's
// kRiverSeaLevelMm -- with zero references anywhere while channel.cpp, in the
// same subsystem, tested against a bare 0 twice. These tests are what stop
// that from happening again on the C++ side; the Python/HLSL mirrors are
// checked by terrain-service/tests/test_sea_level_contract.py, which parses
// the headers because CI has no C++ toolchain.
//
// The VALUE being zero is not the interesting part and is not what most of
// this asserts. What it asserts is that every sea-level site is now DERIVED
// from the one symbol, so a datum change moves all of them together.

#include "voxelcore/core.h"

#include <cstdint>

#include "voxelcore/caves.h"
#include "voxelcore/rivercouple.h"
#include "voxelcore/rivernet.h"
#include "voxelcore/tilestore.h"
#include "vxctest.h"

using namespace vxc;

VXC_TEST(SeaLevelIsWholeVoxels) {
    // The static_assert in core.h already refuses a fractional datum at
    // compile time; this is the runtime statement of the same invariant, so a
    // future change that deletes the assert still fails something.
    CHECK_EQ(int(kSeaLevelVoxelZ * kVoxelSizeMm), int(kSeaLevelMm));
}

VXC_TEST(SeaLevelConstantsAllDeriveFromOneSymbol) {
    // caves.h: "never carve at or below sea level".
    CHECK_EQ(int64_t(kCaveMinVoxelZ), kSeaLevelVoxelZ);
    // rivernet.h: the graph's terminal sink.
    CHECK_EQ(int(kRiverSeaLevelMm), int(kSeaLevelMm));
    // rivercouple.h: still a settable field (synthetic test worlds put their
    // sea elsewhere, and that is exactly the case a shared constant must not
    // override) -- but its DEFAULT is the datum.
    RiverCoupleConfig cfg;
    CHECK_EQ(int64_t(cfg.seaLevelVz), kSeaLevelVoxelZ);
}

VXC_TEST(MissingTileReadsAsSeaLevelNotAsZero) {
    // A tile that was never loaded answers with the DATUM: absent data is
    // open ocean, the same posture terrain-service's MISSING_ELEVATION_M
    // takes on the bake side. The distinction matters because the two used to
    // be independent zeros that could drift apart silently.
    TileGridSampler sampler(20260719, /*scale=*/1);
    const int32_t before = int32_t(sampler.missingTileQueries.load());
    CHECK_EQ(int(sampler.elevationMm(1'000'000, 1'000'000)), int(kSeaLevelMm));
    // ... and it is still COUNTED, so "sea level" never hides a load failure.
    CHECK(int32_t(sampler.missingTileQueries.load()) == before + 1);
}
