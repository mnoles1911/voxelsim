// M4 cave pass (voxelcore/caves.h): determinism, golden digest, the three
// safety rules (bedrock floor, implicit ocean, roof thickness), the
// segment-cap headroom, and â€” the point of the whole formulation â€”
// CONNECTIVITY EVIDENCE from an actual flood fill rather than an assertion
// that noise "probably" joins up.
//
// The connectivity tests flood-fill a DECIMATED sample grid (every 4th voxel,
// i.e. a 0.4 m lattice) instead of every voxel: the narrowest tube is 2.4 m
// across, so a tube cross-section is still >= 6 samples wide and 6-connected
// paths survive decimation, while the analysis box stays inside a sane test
// memory/time budget. connectivity.h's findComponents is coordinate-agnostic,
// so the sample grid is passed to it as if it were a voxel grid.

#include <cstdio>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/cave_families.h"
#include "voxelcore/caves.h"
#include "voxelcore/connectivity.h"
#include "voxelcore/generator.h"
#include "vxctest.h"

using namespace vxc;

namespace {

constexpr uint64_t kSeed = 20260719;

// A flat synthetic column, used by the tests that want to look at the tunnel
// NETWORK on its own with no terrain draping and no clipping in the way.
constexpr int32_t kFlatSurfaceMm = 100000;   // 100 m â€” well clear of sea level
constexpr int32_t kFlatBedrockMm = 45000;    // 45 m â€” deliberately kept at the
                                             // PRE-v5 amplifier band (bedrock
                                             // moved to 180-220 m at v5), so
                                             // this file keeps exercising the
                                             // bedrock clamp against a floor
                                             // shallow enough for it to bind

// The surfaceAt callback (caves.h caveEntranceSite) for the flat world: every
// xy has the same ground, which is what makes these tests measure the NETWORK
// and not the terrain. On flat ground the v25 entrance cavity degenerates to a
// symmetric bowl over its throat -- the hillside-mouth case needs real relief
// and is tested against the amplifier further down.
constexpr auto kFlatSurfaceAt = [](int64_t, int64_t) -> int32_t { return kFlatSurfaceMm; };

// Carve predicate for the flat world.
bool flatCarve(int64_t vx, int64_t vy, int64_t vz) {
    const CaveColumn c = caveColumnFor(kSeed, vx, vy, kFlatSurfaceMm, kFlatSurfaceAt);
    return caveCarveAt(c, kFlatSurfaceMm, kFlatBedrockMm, vz);
}

// The first gated-open ENTRANCE node at or after lattice (i0, j0), scanning the
// same (j, i) order everywhere in this file so two tests never disagree about
// which site "the first one" is.
//
// WHY EVERY ENTRANCE TEST GOES THROUGH THIS. Entrance candidates sit on a
// 102.4 m lattice and only one in four is open, so a test that samples a
// "reasonable" 50-100 m box usually contains NO entrance and then measures the
// box instead of the world. That has now happened four separate times in this
// subsystem -- twice in this file, once in vxc_caveprobe's bring-up, and once
// in vxc_gpu's cave-band fixture, whose comment asserted an entrance was inside
// it for a whole worldgen version while the harness compared none.
bool firstOpenEntranceNode(int64_t i0, int64_t j0, int64_t span, int64_t& iOut, int64_t& jOut) {
    for (int64_t j = j0; j <= j0 + span; j += 4)
        for (int64_t i = i0; i <= i0 + span; i += 4)
            if (((hash2(kSeed, i, j, CH_CAVE_SHAFT) >> 48) & kCaveShaftGateMask) == 0) {
                iOut = i;
                jOut = j;
                return true;
            }
    return false;
}

struct ComponentStats {
    int32_t count = 0;
    size_t total = 0;
    size_t largest = 0;
    size_t secondLargest = 0;
};

ComponentStats summarize(const ConnectivityResult& r) {
    ComponentStats s;
    s.count = r.componentCount;
    for (const Component& c : r.components) {
        s.total += c.size();
        if (c.size() > s.largest) {
            s.secondLargest = s.largest;
            s.largest = c.size();
        } else if (c.size() > s.secondLargest) {
            s.secondLargest = c.size();
        }
    }
    return s;
}

} // namespace

// --- determinism ------------------------------------------------------------

VXC_TEST(cave_column_is_deterministic) {
    for (int64_t x = -4000; x <= 4000; x += 613)
        for (int64_t y = -4000; y <= 4000; y += 419) {
            const CaveColumn a = caveColumnFor(kSeed, x, y, kFlatSurfaceMm, kFlatSurfaceAt);
            const CaveColumn b = caveColumnFor(kSeed, x, y, kFlatSurfaceMm, kFlatSurfaceAt);
            CHECK_EQ(a.count, b.count);
            for (int32_t s = 0; s < a.count; ++s) {
                CHECK_EQ(a.segs[s].marginSq, b.segs[s].marginSq);
                CHECK_EQ(a.segs[s].depthMm, b.segs[s].depthMm);
            }
        }
}

VXC_TEST(cave_carve_is_deterministic_through_the_amplifier) {
    SyntheticTileSampler tilesA(kSeed), tilesB(kSeed);
    Amplifier a(kSeed, tilesA), b(kSeed, tilesB);
    size_t air = 0;
    for (int64_t x = -200; x <= 200; x += 17)
        for (int64_t y = -200; y <= 200; y += 19) {
            const ColumnSample ca = a.column(x, y), cb = b.column(x, y);
            CHECK_EQ(ca.cave.count, cb.cave.count);
            const int64_t topVz = floorDiv(ca.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            for (int64_t vz = topVz; vz > topVz - 400; --vz) {
                const MaterialId ma = Amplifier::materialAt(ca, vz);
                CHECK_EQ(ma, Amplifier::materialAt(cb, vz));
                if (ma == MAT_AIR) ++air;
            }
        }
    // A different seed must produce a different carve (channel separation is
    // real, not accidentally seed-independent).
    SyntheticTileSampler tilesC(kSeed + 1);
    Amplifier c(kSeed + 1, tilesC);
    bool differs = false;
    for (int64_t x = -200; x <= 200 && !differs; x += 17)
        for (int64_t y = -200; y <= 200 && !differs; y += 19) {
            const CaveColumn ac = caveColumnFor(kSeed, x, y, kFlatSurfaceMm, kFlatSurfaceAt);
            const CaveColumn cc = caveColumnFor(kSeed + 1, x, y, kFlatSurfaceMm, kFlatSurfaceAt);
            if (ac.count != cc.count) differs = true;
            else
                for (int32_t s = 0; s < ac.count; ++s)
                    if (ac.segs[s].depthMm != cc.segs[s].depthMm) differs = true;
        }
    CHECK(differs);
    CHECK(air > 0); // the sampled slab really does contain carved cave air
}

// --- golden digest ----------------------------------------------------------

VXC_TEST(cave_golden_digest) {
    // Digests the cave layer ALONE (the carved/not-carved bit per voxel plus
    // the per-column segment reduction), so it moves only when the cave math
    // moves â€” unlike the mips/GPU digests, which also move for any
    // stratigraphy change.
    Digest d;
    for (int64_t y = -640; y < 640; y += 37) {
        for (int64_t x = -640; x < 640; x += 41) {
            const CaveColumn c = caveColumnFor(kSeed, x, y, kFlatSurfaceMm, kFlatSurfaceAt);
            d.u32(static_cast<uint32_t>(c.count));
            for (int32_t s = 0; s < c.count; ++s) {
                d.u32(static_cast<uint32_t>(c.segs[s].marginSq));
                d.u32(static_cast<uint32_t>(c.segs[s].depthMm));
            }
            d.u32(static_cast<uint32_t>(c.shaftMarginSq));
            d.u32(static_cast<uint32_t>(c.shaftDepthMinMm));
            d.u32(static_cast<uint32_t>(c.shaftDepthMaxMm));
            for (int64_t vz = 560; vz < 1000; vz += 3)
                d.u8(caveCarveAt(c, kFlatSurfaceMm, kFlatBedrockMm, vz) ? 1 : 0);
        }
    }
    // AND A SWEEP THAT PROVABLY CONTAINS AN ENTRANCE (v25).
    //
    // The 128 m grid above is a TUNNEL golden and nothing else: entrance nodes
    // are 102.4 m apart and 1-in-4 gated, so it contains one about a third of
    // the time and contained none at this seed. The v25 entrance rework changed
    // the entrance construct completely -- new cavity, new field, new per-voxel
    // compare -- and this digest did not move, which is the same blind spot
    // vxc_gpu's cave-band fixture had. Fixed by sweeping a real site's own
    // footprint, and by CHECKING coverage below rather than trusting a comment.
    int64_t ei = 0, ej = 0;
    CHECK(firstOpenEntranceNode(0, 0, 64, ei, ej));
    const CaveNode enode = caveNode(kSeed, ei, ej);
    const int64_t envx = floorDiv(enode.xMm, int64_t(kVoxelSizeMm));
    const int64_t envy = floorDiv(enode.yMm, int64_t(kVoxelSizeMm));
    int64_t entranceColumnsDigested = 0;
    for (int64_t dy = -140; dy <= 140; dy += 7)
        for (int64_t dx = -140; dx <= 140; dx += 7) {
            const CaveColumn c =
                caveColumnFor(kSeed, envx + dx, envy + dy, kFlatSurfaceMm, kFlatSurfaceAt);
            if (c.shaftMarginSq > 0) ++entranceColumnsDigested;
            d.u32(static_cast<uint32_t>(c.shaftMarginSq));
            d.u32(static_cast<uint32_t>(c.shaftDepthMinMm));
            d.u32(static_cast<uint32_t>(c.shaftDepthMaxMm));
            for (int64_t vz = 560; vz < 1000; vz += 3)
                d.u8(caveCarveAt(c, kFlatSurfaceMm, kFlatBedrockMm, vz) ? 1 : 0);
        }
    // The digest is only an entrance golden if entrance columns went into it.
    CHECK(entranceColumnsDigested > 0);
    std::printf("    [caves] golden covers %lld entrance columns at lattice (%lld,%lld)\n",
                (long long)entranceColumnsDigested, (long long)ei, (long long)ej);
    // GOLDEN(cave_layer) â€” new at kWorldGenVersion 4; MOVED for M4 cave pass v2
    // (docs/cavern-design.md C2): caveColumnFor now also emits crevice segs on
    // ~1-in-8 of existing edges, so count/segs[] shift for many sampled
    // columns even though the tunnel geometry itself is unchanged. Re-pinned
    // per the build plan's explicit "caves.h + test_caves.cpp headroom/golden
    // updates" ownership for this subtask. MOVED AGAIN at kWorldGenVersion 24
    // (docs/underground-system-plan.md W2): every edge axis is now a two-segment
    // polyline through a hash-jittered waypoint and its radius interpolates
    // between three control values instead of being one constant, so every
    // marginSq and depthMm in the reduction moves. Verified independently of
    // this pin by vxc_caveprobe (plan-view and cross-section images plus the
    // per-family census) and by vxc_gpu, whose new cave-band fixture compares
    // the cave voxels themselves for the first time.
    CHECK_EQ(d.h, 0x6FF4E353EA1E63E4ull);
}

// --- safety rule 1: the bedrock floor is never breached ----------------------

VXC_TEST(cave_never_breaches_bedrock) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    size_t carved = 0;
    int64_t closestToBedrockMm = 1 << 30;
    for (int64_t x = -512; x <= 512; x += 7)
        for (int64_t y = -512; y <= 512; y += 11) {
            const ColumnSample col = amp.column(x, y);
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            for (int64_t vz = topVz; vz > topVz - 800; --vz) {
                const MaterialId strat = Amplifier::stratigraphyAt(col, vz);
                const MaterialId m = Amplifier::materialAt(col, vz);
                // Bedrock is never turned into air, by any path.
                if (strat == MAT_BEDROCK) CHECK_EQ(m, MAT_BEDROCK);
                if (m == MAT_AIR && strat != MAT_AIR) {
                    ++carved;
                    const int64_t depthMm =
                        int64_t(col.surfaceMm) - (vz * kVoxelSizeMm + kVoxelSizeMm / 2);
                    const int64_t gap = int64_t(col.bedrockDepthMm) - depthMm;
                    if (gap < closestToBedrockMm) closestToBedrockMm = gap;
                }
            }
        }
    CHECK(carved > 0);
    CHECK(closestToBedrockMm >= kCaveBedrockMarginMm);
    std::printf("    [caves] bedrock: %zu carved voxels sampled, closest approach to "
                "bedrock top %lld mm (margin %lld mm)\n",
                carved, static_cast<long long>(closestToBedrockMm),
                static_cast<long long>(kCaveBedrockMarginMm));
}

VXC_TEST(cave_bedrock_margin_clamp_refuses_a_shallow_bedrock_column) {
    // The geometry alone already keeps caves 3.2 m above the shallowest
    // bedrock the amplifier can make (caves.h static_assert), so the runtime
    // clamp is a backstop that production data never exercises. Drive it
    // directly with a column whose bedrock is unnaturally shallow: every
    // otherwise-carveable voxel must be refused.
    int64_t tested = 0;
    for (int64_t x = 0; x < 512; x += 3)
        for (int64_t y = 0; y < 512; y += 5) {
            const CaveColumn c = caveColumnFor(kSeed, x, y, kFlatSurfaceMm, kFlatSurfaceAt);
            if (c.count == 0) continue;
            for (int64_t vz = 560; vz < 960; ++vz) {
                if (!caveCarveAt(c, kFlatSurfaceMm, kFlatBedrockMm, vz)) continue;
                ++tested;
                // Same voxel, same tubes, bedrock top at 8 m: refused.
                CHECK(!caveCarveAt(c, kFlatSurfaceMm, 8000, vz));
            }
        }
    CHECK(tested > 0);
}

// --- safety rule 2: the implicit ocean is never breached ---------------------

VXC_TEST(cave_never_carves_at_or_below_sea_level_or_in_coastal_columns) {
    // Directly driven: SyntheticTileSampler's terrain near the origin sits
    // around +1.1 km, so a world scan alone would never reach a coastal
    // column. The ocean guard is a property of the definition, so exercise it
    // on the definition â€” every surface height from a deep seafloor up through
    // the beach band to the cave threshold.
    size_t oceanColumns = 0;
    for (int32_t surfaceMm = -40000; surfaceMm < kCaveMinSurfaceMm; surfaceMm += 271)
        for (int64_t x = 0; x < 256; x += 37)
            for (int64_t y = 0; y < 256; y += 41) {
                const CaveColumn c = caveColumnFor(kSeed, x, y, surfaceMm, kFlatSurfaceAt);
                ++oceanColumns;
                CHECK_EQ(c.count, 0);
                CHECK_EQ(c.shaftMarginSq, 0);
                for (int64_t vz = -600; vz < 200; vz += 11)
                    CHECK(!caveCarveAt(c, surfaceMm, 45000, vz));
            }
    // Above the threshold tubes do exist, but never at or below sea level.
    size_t subSeaChecks = 0;
    for (int32_t surfaceMm = kCaveMinSurfaceMm; surfaceMm < 60000; surfaceMm += 311)
        for (int64_t x = 0; x < 256; x += 19)
            for (int64_t y = 0; y < 256; y += 23) {
                const CaveColumn c = caveColumnFor(kSeed, x, y, surfaceMm, kFlatSurfaceAt);
                for (int64_t vz = -400; vz < kCaveMinVoxelZ; vz += 3) {
                    CHECK(!caveCarveAt(c, surfaceMm, 45000, vz));
                    ++subSeaChecks;
                }
            }
    CHECK(oceanColumns > 0);
    CHECK(subSeaChecks > 0);

    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    size_t carved = 0;
    int64_t lowestCarvedVz = 1 << 30;
    for (int64_t x = -3000; x <= 3000; x += 149)
        for (int64_t y = -3000; y <= 3000; y += 151) {
            const ColumnSample col = amp.column(x, y);
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            for (int64_t vz = topVz; vz > topVz - 600; --vz) {
                if (Amplifier::stratigraphyAt(col, vz) == MAT_AIR) continue;
                if (Amplifier::materialAt(col, vz) != MAT_AIR) continue;
                ++carved;
                CHECK(vz >= kCaveMinVoxelZ);
                CHECK(col.surfaceMm >= kCaveMinSurfaceMm);
                if (vz < lowestCarvedVz) lowestCarvedVz = vz;
            }
        }
    CHECK(carved > 0);
    CHECK(lowestCarvedVz >= kCaveMinVoxelZ);
    std::printf("    [caves] ocean: %zu synthetic sub-threshold columns produced zero "
                "tubes and zero carves; %zu sub-sea-level probes all refused; lowest "
                "carved voxel in the world scan z = %lld (sea level = 0)\n",
                oceanColumns, subSeaChecks, static_cast<long long>(lowestCarvedVz));
}

// --- safety rule 3: surface integrity / roof thickness -----------------------

VXC_TEST(cave_surface_integrity_roof_thickness) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    int64_t thinnestRoofMm = 1 << 30;
    size_t carved = 0, columnsWithCaves = 0, columnsSampled = 0, shaftColumns = 0;
    size_t perforatedColumns = 0;
    // CENTRED ON A REAL ENTRANCE (v25), not on the world origin. This test's
    // whole subject is "the roof clamp holds everywhere except at the
    // enumerated entrances", and at the origin it sampled ZERO entrance
    // columns for the entire life of the cave pass -- so the exception half of
    // its own statement was never exercised and its perforation bound was a
    // bound on nothing. See firstOpenEntranceNode for why this keeps happening.
    int64_t ei = 0, ej = 0;
    CHECK(firstOpenEntranceNode(0, 0, 64, ei, ej));
    const CaveNode enode = caveNode(kSeed, ei, ej);
    const int64_t ex0 = floorDiv(enode.xMm, int64_t(kVoxelSizeMm));
    const int64_t ey0 = floorDiv(enode.yMm, int64_t(kVoxelSizeMm));
    for (int64_t x = ex0 - 512; x <= ex0 + 512; x += 3)
        for (int64_t y = ey0 - 512; y <= ey0 + 512; y += 5) {
            const ColumnSample col = amp.column(x, y);
            ++columnsSampled;
            // Sinkhole shafts are the one deliberate roof breach (caves.h
            // kCaveShaftNodeMask); their footprint is measured separately
            // below, and it is what makes the network enterable at all.
            if (col.cave.shaftMarginSq > 0) {
                ++shaftColumns;
                // Inside an entrance FOOTPRINT is not the same as open to the
                // sky, and v25 is exactly the change that separated them: the
                // cavity's roof is clipped by the ground, so most of a doline's
                // footprint carries rock overhead (its overhanging lip) and a
                // hillside mouth is roofed over its whole length. Perforation
                // -- "you would fall in walking over it" -- is the number the
                // "the surface is not a colander" guarantee is about, and it is
                // now a strict SUBSET of the footprint rather than equal to it.
                if (col.cave.shaftDepthMinMm == 0 &&
                    Amplifier::materialAt(
                        col, floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm)) == MAT_AIR)
                    ++perforatedColumns;
                continue;
            }
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            bool any = false;
            // The topmost kCaveRoofMinMm below the surface must be intact, and
            // the surface voxel itself must never be carved.
            for (int64_t vz = topVz; vz > topVz - 800; --vz) {
                if (Amplifier::stratigraphyAt(col, vz) == MAT_AIR) continue;
                if (Amplifier::materialAt(col, vz) != MAT_AIR) continue;
                any = true;
                ++carved;
                const int64_t roofMm =
                    int64_t(col.surfaceMm) - (vz * kVoxelSizeMm + kVoxelSizeMm / 2);
                if (roofMm < thinnestRoofMm) thinnestRoofMm = roofMm;
            }
            if (any) ++columnsWithCaves;
        }
    CHECK(carved > 0);
    CHECK(thinnestRoofMm >= kCaveRoofMinMm);
    // Not swiss cheese: most columns are not undermined at all.
    CHECK(columnsWithCaves * 2 < columnsSampled);
    // THE ROOF-INTEGRITY GUARANTEE, in the two halves v25 split it into.
    //
    // (a) The exception is still SPARSE and still enumerated: the fraction of
    //     ground lying over an entrance cavity is bounded. It is no longer
    //     point-sized -- a doline is a landform, not a drill hole -- so the
    //     bound is 5% of a footprint centred on an entrance, not the 0.2% a
    //     1.4 m bore needed. What has NOT changed is that it is a closed set of
    //     hash-gated sites and not a threshold that can leak.
    // (b) Most of that footprint is ROOFED. The thing a player falls into is
    //     a minority of the cavity, not the whole of it -- which is the
    //     difference between a doline with an overhanging lip and a hole.
    //
    // BOTH NUMBERS HERE ARE LOCAL DENSITIES, not world rates, and the bounds
    // are written knowing it: this footprint is deliberately centred on an
    // entrance, so it contains one where an average 102 m square contains a
    // quarter of one. The GLOBAL "the surface is not a colander" rate is
    // asserted where it belongs, over undirected ground, in
    // cave_per_family_volume_and_entrance_census.
    CHECK(shaftColumns * 20 < columnsSampled);
    CHECK(perforatedColumns * 50 < columnsSampled);
    CHECK(perforatedColumns * 2 < shaftColumns);
    // ...and the entrance really is inside this footprint, or neither bound
    // above means anything. That is the failure mode this test shipped with.
    CHECK(shaftColumns > 0);
    CHECK(perforatedColumns > 0);
    std::printf("    [caves] roof: thinnest cover %lld mm (clamp %lld mm) over "
                "non-entrance ground; %zu/%zu sampled columns (%.1f%%) have any cave "
                "beneath them; %zu (%.4f%%) lie over an entrance cavity, of which %zu are open\n",
                static_cast<long long>(thinnestRoofMm),
                static_cast<long long>(kCaveRoofMinMm), columnsWithCaves, columnsSampled,
                100.0 * double(columnsWithCaves) / double(columnsSampled), shaftColumns,
                100.0 * double(shaftColumns) / double(columnsSampled), perforatedColumns);
}

// --- storage bound ----------------------------------------------------------

VXC_TEST(cave_segment_cap_headroom) {
    // kMaxCaveSegs truncates deterministically if it is ever hit, but it must
    // not be hit: this measures the real maximum over a wide sample.
    int32_t maxSegs = 0;
    size_t columns = 0, withAny = 0;
    for (int64_t x = -2048; x < 2048; x += 3)
        for (int64_t y = -2048; y < 2048; y += 7) {
            const CaveColumn c = caveColumnFor(kSeed, x, y, kFlatSurfaceMm, kFlatSurfaceAt);
            ++columns;
            if (c.count > 0) ++withAny;
            if (c.count > maxSegs) maxSegs = c.count;
        }
    CHECK(maxSegs > 0);
    CHECK(maxSegs < kMaxCaveSegs); // strict: the cap must never bind
    std::printf("    [caves] segment cap: max %d tubes per column over %zu columns "
                "(cap %d); %.1f%% of columns are over a tube\n",
                maxSegs, columns, kMaxCaveSegs, 100.0 * double(withAny) / double(columns));
}

// --- crevices (M4 cave pass v2, docs/cavern-design.md Â§4) -------------------

VXC_TEST(crevice_gate_rate_is_about_one_in_eight) {
    // Direct hash-level check, independent of whether any particular edge
    // exists: the gate itself should open close to 1/8 of the time.
    int64_t total = 0, open = 0;
    for (int64_t i = -400; i < 400; ++i)
        for (int64_t j = -400; j < 400; j += 7)
            for (int32_t dir = 0; dir < 2; ++dir) {
                ++total;
                if (caveCreviceGateOpen(caveCreviceHash(kSeed, i, j, dir))) ++open;
            }
    const double rate = double(open) / double(total);
    std::printf("    [caves] crevice gate: %lld/%lld open (%.3f%%, target 12.5%%)\n",
                static_cast<long long>(open), static_cast<long long>(total), 100.0 * rate);
    CHECK(rate > 0.10);
    CHECK(rate < 0.15);
}

VXC_TEST(crevice_geometry_pinches_out_at_nodes_and_contains_the_tube_axis) {
    // Recompute the crevice reduction directly (same formulas as
    // caveColumnFor's crevice block) for a handful of real gated-open edges,
    // and check the two structural claims the header comment makes: the
    // emitted z-interval always contains the tube axis depth `cd` at
    // mid-edge (u=0.5, where the lens taper is maximal), and every decoded
    // field stays inside its documented range.
    int64_t edgesChecked = 0;
    for (int64_t j = 0; j <= 40 && edgesChecked < 20; j += 4) {
        for (int64_t i = 0; i <= 40 && edgesChecked < 20; ++i) {
            for (int32_t dir = 0; dir < 2 && edgesChecked < 20; ++dir) {
                if (!caveEdgeExists(kSeed, i, j, dir)) continue;
                const uint64_t crevH = caveCreviceHash(kSeed, i, j, dir);
                if (!caveCreviceGateOpen(crevH)) continue;
                ++edgesChecked;

                const CaveNode a = caveNode(kSeed, i, j);
                const CaveNode b = (dir == 0) ? caveNode(kSeed, i + 1, j) : caveNode(kSeed, i, j + 1);
                const int64_t dx = b.xMm - a.xMm, dy = b.yMm - a.yMm;
                const int64_t den = dx * dx + dy * dy;
                CHECK(den > 0);

                const int64_t tMm =
                    kCrevHalfThickMinMm +
                    static_cast<int64_t>(((crevH & 0xFFFFFu) * static_cast<uint64_t>(kCrevHalfThickSpanMm)) >> 20);
                const int64_t hUpMm =
                    kCrevUpMinMm + static_cast<int64_t>((((crevH >> 20) & 0xFFFFFu) *
                                                          static_cast<uint64_t>(kCrevUpSpanMm)) >> 20);
                const int64_t hDownMm =
                    kCrevDownMinMm + static_cast<int64_t>((((crevH >> 40) & 0xFFFFFu) *
                                                            static_cast<uint64_t>(kCrevDownSpanMm)) >> 20);
                CHECK(tMm >= kCrevHalfThickMinMm);
                CHECK(tMm < kCrevHalfThickMinMm + kCrevHalfThickSpanMm);
                CHECK(hUpMm >= kCrevUpMinMm);
                CHECK(hUpMm < kCrevUpMinMm + kCrevUpSpanMm);
                CHECK(hDownMm >= kCrevDownMinMm);
                CHECK(hDownMm < kCrevDownMinMm + kCrevDownSpanMm);

                // Mid-EDGE is the WAYPOINT since v24, not the linear midpoint:
                // u = 1 there by construction (sub = 1, num = 0), so the taper
                // is maximal, the slab is at its full roof-clamped size, and it
                // must contain the axis depth at that same point -- which is
                // now the waypoint's own dipped depth, not an interpolation
                // between the two nodes.
                const CaveWaypoint w = caveWaypoint(kSeed, i, j, dir, a, b);
                const int64_t cdMid = w.depthMm;
                const int64_t hUpEffMm = clampi64(hUpMm, 0, cdMid - kCaveRoofMinMm);
                const int64_t halfSpanMm = (hUpEffMm + hDownMm) / 2;
                // u at the waypoint: sub = 1, num = 0 -> (1<<16 + 0)/2 = 0.5,
                // which is where 4u(1-u) peaks at 1.0.
                const int64_t uQ16Mid =
                    floorDiv((static_cast<int64_t>(1) << 16) + 0, kCaveEdgeSubSegs);
                const int64_t taperQ16Mid = (4 * uQ16Mid * ((1 << 16) - uQ16Mid)) >> 16;
                const int64_t halfSpanTaperedMid = floorDiv(halfSpanMm * taperQ16Mid, 1 << 16);
                CHECK(halfSpanTaperedMid > 0);
                const int64_t depthMid = cdMid + floorDiv(hDownMm - hUpEffMm, 2);
                const int64_t centerOffset = depthMid - cdMid;
                CHECK(centerOffset <= halfSpanTaperedMid);
                CHECK(-centerOffset <= halfSpanTaperedMid);

                // At either node the taper is zero by construction -- the slab
                // pinches to nothing. Node A is sub=0, num=0 -> u=0; node B is
                // sub=1, num=den -> u = (1<<16 + 1<<16)/2 = 1<<16.
                const int64_t uQ16NodeA = floorDiv(0 + floorDiv(0, den), kCaveEdgeSubSegs);
                CHECK_EQ((4 * uQ16NodeA * ((1 << 16) - uQ16NodeA)) >> 16, 0);
                const int64_t uQ16NodeB =
                    floorDiv((static_cast<int64_t>(1) << 16) + floorDiv(den << 16, den),
                             kCaveEdgeSubSegs);
                CHECK_EQ(uQ16NodeB, static_cast<int64_t>(1) << 16);
                CHECK_EQ((4 * uQ16NodeB * ((1 << 16) - uQ16NodeB)) >> 16, 0);
            }
        }
    }
    std::printf("    [caves] crevice geometry: %lld gated-open edges sampled, all "
                "well-formed and mid-edge slabs contain their tube axis\n",
                static_cast<long long>(edgesChecked));
    CHECK(edgesChecked > 0);
}

VXC_TEST(crevice_segments_actually_appear_in_caveColumnFor) {
    // End-to-end: scanning real columns must find at least one column whose
    // segment count exceeds what four tunnels alone could ever contribute at
    // an ordinary junction, evidencing a crevice seg riding along.
    int32_t maxSegs = 0;
    size_t columns = 0;
    for (int64_t x = -4096; x < 4096; x += 3)
        for (int64_t y = -4096; y < 4096; y += 5) {
            ++columns;
            const CaveColumn c = caveColumnFor(kSeed, x, y, kFlatSurfaceMm, kFlatSurfaceAt);
            if (c.count > maxSegs) maxSegs = c.count;
        }
    std::printf("    [caves] crevice presence: max %d segs/column over %zu columns "
                "(cap %d) -- above the pre-crevice max of 4 confirms crevices fire\n",
                maxSegs, columns, kMaxCaveSegs);
    CHECK(maxSegs > 4);
    CHECK(maxSegs < kMaxCaveSegs);
}

// --- connectivity: the tunnel network on its own -----------------------------

VXC_TEST(cave_network_is_one_connected_component_in_flat_terrain) {
    // Structural claim under test: the backbone edges (every 4th lattice row's
    // +x edges, every 4th column's +y edges) form a connected grid graph for
    // every seed, so the union of tubes is one connected passage system. With
    // flat terrain there is no depth-space distortion, so this isolates the
    // graph from the draping.
    constexpr int64_t kStep = 4;      // 0.4 m sample lattice
    constexpr int64_t kNXY = 384;     // 384 * 4 = 1536 voxels = 153.6 m = 6 lattice cells
    constexpr int64_t kNZ = 108;      // deepest legal carve up through the surface
    constexpr int64_t kZ0 = 570;      // 100 m surface - 43 m == deepest legal carve

    const ConnectivityResult r = findComponents(
        [&](int64_t a, int64_t b, int64_t c) {
            return flatCarve(a * kStep, b * kStep, kZ0 + c * kStep);
        },
        VoxelCoord{0, 0, 0}, VoxelCoord{kNXY - 1, kNXY - 1, kNZ - 1});

    const ComponentStats s = summarize(r);
    CHECK(s.total > 0);
    CHECK(s.count > 0);
    const double largestShare = double(s.largest) / double(s.total);
    std::printf("    [caves] flat-terrain network over %lld x %lld x %lld samples: "
                "%d component(s), %zu cave samples, largest = %zu (%.2f%%), "
                "2nd = %zu\n",
                static_cast<long long>(kNXY), static_cast<long long>(kNXY),
                static_cast<long long>(kNZ), s.count, s.total, s.largest,
                100.0 * largestShare, s.secondLargest);
    // One dominant network, not a bubble bath. The remainder is border
    // clipping: tubes that enter the analysis box and leave again without
    // meeting the backbone inside it.
    CHECK(largestShare > 0.80);
    CHECK(s.count < 32);
}

VXC_TEST(cave_network_stays_connected_under_real_terrain) {
    // The real thing: caves carved under SyntheticTileSampler terrain, where
    // depth-space draping can pinch a tunnel wherever the surface drops faster
    // than a tube is wide. Measured, not assumed.
    constexpr int64_t kStep = 4;
    constexpr int64_t kNXY = 256;  // 1024 voxels = 102.4 m = 4 lattice cells
    constexpr int64_t kX0 = -512, kY0 = -512;

    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);

    // Cache one ColumnSample per sampled column (findComponents calls solidFn
    // once per sample, but recomputing column() per sample would dominate).
    std::vector<ColumnSample> cols(size_t(kNXY) * size_t(kNXY));
    int64_t minSurfaceMm = INT64_MAX, maxSurfaceMm = INT64_MIN;
    for (int64_t b = 0; b < kNXY; ++b)
        for (int64_t a = 0; a < kNXY; ++a) {
            const ColumnSample c = amp.column(kX0 + a * kStep, kY0 + b * kStep);
            cols[size_t(a + kNXY * b)] = c;
            if (c.surfaceMm < minSurfaceMm) minSurfaceMm = c.surfaceMm;
            if (c.surfaceMm > maxSurfaceMm) maxSurfaceMm = c.surfaceMm;
        }
    // z window covering every legally carveable voxel in the footprint.
    const int64_t z0 = floorDiv(minSurfaceMm - 45000, kVoxelSizeMm);
    const int64_t z1 = floorDiv(maxSurfaceMm, kVoxelSizeMm); // include sinkhole mouths
    const int64_t nz = (z1 - z0) / kStep + 1;
    CHECK(nz > 0);

    const ConnectivityResult r = findComponents(
        [&](int64_t a, int64_t b, int64_t c) {
            const ColumnSample& col = cols[size_t(a + kNXY * b)];
            const int64_t vz = z0 + c * kStep;
            return Amplifier::materialAt(col, vz) == MAT_AIR &&
                   Amplifier::stratigraphyAt(col, vz) != MAT_AIR;
        },
        VoxelCoord{0, 0, 0}, VoxelCoord{kNXY - 1, kNXY - 1, nz - 1});

    const ComponentStats s = summarize(r);
    CHECK(s.total > 0);
    const double largestShare = double(s.largest) / double(s.total);
    std::printf("    [caves] real-terrain network over %lld x %lld x %lld samples "
                "(surface %lld..%lld mm): %d component(s), %zu cave samples, "
                "largest = %zu (%.2f%%), 2nd = %zu\n",
                static_cast<long long>(kNXY), static_cast<long long>(kNXY),
                static_cast<long long>(nz), static_cast<long long>(minSurfaceMm),
                static_cast<long long>(maxSurfaceMm), s.count, s.total, s.largest,
                100.0 * largestShare, s.secondLargest);
    // Far from "thousands of bubbles": a handful of components, one dominant.
    CHECK(largestShare > 0.50);
    CHECK(s.count < 64);
    // Sanity: the components are large, not single-voxel specks.
    CHECK(s.largest > 1000);
}

VXC_TEST(cave_sinkhole_reaches_the_surface_and_joins_the_main_network) {
    // The entrance claim, end to end: find the first gated sinkhole node, then
    // flood-fill a box around it in flat terrain and require that the SURFACE
    // opening and a tunnel voxel tens of metres away are the same component.
    int64_t si = 0, sj = 0;
    bool found = false;
    for (int64_t j = 0; j <= 64 && !found; j += 4)
        for (int64_t i = 0; i <= 64 && !found; i += 4)
            if ((((hash2(kSeed, i, j, CH_CAVE_SHAFT) >> 48) & kCaveShaftGateMask) == 0)) {
                si = i;
                sj = j;
                found = true;
            }
    CHECK(found);
    const CaveNode node = caveNode(kSeed, si, sj);

    // The shaft column: carved from the surface all the way down to the node.
    const int64_t nvx = floorDiv(node.xMm, int64_t(kVoxelSizeMm));
    const int64_t nvy = floorDiv(node.yMm, int64_t(kVoxelSizeMm));
    const CaveColumn shaftCol = caveColumnFor(kSeed, nvx, nvy, kFlatSurfaceMm, kFlatSurfaceAt);
    CHECK(shaftCol.shaftMarginSq > 0);
    CHECK(shaftCol.count > 0); // backbone tunnels meet at a shaft node, by construction
    const int64_t topVz = floorDiv(kFlatSurfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
    CHECK(caveCarveAt(shaftCol, kFlatSurfaceMm, kFlatBedrockMm, topVz)); // daylight
    const int64_t nodeVz = floorDiv(kFlatSurfaceMm - node.depthMm, int64_t(kVoxelSizeMm));
    CHECK(caveCarveAt(shaftCol, kFlatSurfaceMm, kFlatBedrockMm, nodeVz)); // and the bottom

    // Flood fill a 51.2 m box centred on the shaft, from the deepest legal
    // carve right up to the surface.
    constexpr int64_t kStep = 4;
    constexpr int64_t kNXY = 128;
    const int64_t x0 = nvx - kNXY * kStep / 2, y0 = nvy - kNXY * kStep / 2;
    const int64_t z0 = 570, nz = (topVz - z0) / kStep + 1;
    const ConnectivityResult r = findComponents(
        [&](int64_t a, int64_t b, int64_t c) {
            return flatCarve(x0 + a * kStep, y0 + b * kStep, z0 + c * kStep);
        },
        VoxelCoord{0, 0, 0}, VoxelCoord{kNXY - 1, kNXY - 1, nz - 1});

    // Which component owns the surface opening, and how deep does it go?
    const int64_t sa = (nvx - x0) / kStep, sb = (nvy - y0) / kStep;
    int32_t surfaceComp = -1;
    int64_t deepestInSurfaceComp = 1 << 30;
    size_t surfaceCompSize = 0, total = 0;
    for (size_t ci = 0; ci < r.components.size(); ++ci) {
        total += r.components[ci].size();
        for (const VoxelCoord& v : r.components[ci].voxels) {
            if (v.z == nz - 1 && v.x >= sa - 2 && v.x <= sa + 2 && v.y >= sb - 2 &&
                v.y <= sb + 2) {
                surfaceComp = static_cast<int32_t>(ci);
                surfaceCompSize = r.components[ci].size();
            }
        }
    }
    CHECK(surfaceComp >= 0); // the opening really is in the flood fill
    if (surfaceComp >= 0)
        for (const VoxelCoord& v : r.components[size_t(surfaceComp)].voxels)
            if (v.z < deepestInSurfaceComp) deepestInSurfaceComp = v.z;
    // From daylight down to the tunnel band, in one connected component.
    const int64_t descentM = (nz - 1 - deepestInSurfaceComp) * kStep * kVoxelSizeMm / 1000;
    CHECK(descentM >= 20);
    std::printf("    [caves] sinkhole at lattice (%lld,%lld): entrance component holds "
                "%zu of %zu cave samples and descends %lld m from daylight\n",
                static_cast<long long>(si), static_cast<long long>(sj), surfaceCompSize,
                total, static_cast<long long>(descentM));
}

// --- W3: the entrance portfolio, and the three guarantees it inherited -------
//
// docs/underground-system-plan.md W3. v24's entrance was a naked vertical
// cylinder; the owner called those out by name ("really weird vertical shafts
// that shoot straight up to the game world surface"). v25 replaces the SHAPE
// while keeping the three things the cylinder was carrying, and these tests are
// what stop a later tune quietly dropping one of them.

VXC_TEST(cave_entrance_rate_and_daylight_are_unchanged_from_v24) {
    // GUARANTEE 1 (entrance rate) and GUARANTEE 2 (structural connectivity),
    // both stated over EVERY open site in a wide sample rather than over one.
    //
    // The rate is a property of the gate and the candidate lattice, neither of
    // which v25 touched -- so the test is not "roughly the same", it is
    // "identical to the closed form": one candidate per 4x4 lattice cells, one
    // in four of them open.
    int64_t candidates = 0, opened = 0, daylit = 0, reachesNode = 0;
    for (int64_t j = -128; j <= 128; j += 4)
        for (int64_t i = -128; i <= 128; i += 4) {
            ++candidates;
            if (((hash2(kSeed, i, j, CH_CAVE_SHAFT) >> 48) & kCaveShaftGateMask) != 0) continue;
            ++opened;
            const CaveNode n = caveNode(kSeed, i, j);
            const int64_t nvx = floorDiv(n.xMm, int64_t(kVoxelSizeMm));
            const int64_t nvy = floorDiv(n.yMm, int64_t(kVoxelSizeMm));
            const CaveColumn c = caveColumnFor(kSeed, nvx, nvy, kFlatSurfaceMm, kFlatSurfaceAt);
            const int64_t topVz = floorDiv(kFlatSurfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            // Open to the sky directly over the node...
            if (caveCarveAt(c, kFlatSurfaceMm, kFlatBedrockMm, topVz)) ++daylit;
            // ...and carved CONTINUOUSLY down to the node itself, which is a
            // backbone crossing and therefore on the main component. This is
            // the throat, and it is exactly why v25 kept it: the cavity's
            // arithmetic never has to be trusted for the entrance guarantee.
            const int64_t nodeVz = floorDiv(kFlatSurfaceMm - n.depthMm, int64_t(kVoxelSizeMm));
            bool continuous = true;
            for (int64_t vz = topVz; vz >= nodeVz && continuous; --vz)
                if (!caveCarveAt(c, kFlatSurfaceMm, kFlatBedrockMm, vz)) continuous = false;
            if (continuous) ++reachesNode;
            CHECK(c.count > 0); // backbone tunnels meet here, by construction
        }
    std::printf("    [caves] entrance sites: %lld open of %lld candidates on the 102.4 m grid; "
                "%lld daylit at the axis, %lld carved continuously from daylight to the node\n",
                (long long)opened, (long long)candidates, (long long)daylit,
                (long long)reachesNode);
    CHECK(opened > 0);
    // The gate is 1-in-4 on two bits, so over 4225 candidates the count sits
    // near a quarter. Bounds are wide on purpose: this is a statement about the
    // GATE being untouched, not about the hash's fine balance.
    CHECK(opened * 5 > candidates);
    CHECK(opened * 3 < candidates);
    // EVERY open site is an entrance. Not "most": the throat is unconditional,
    // so one failure here means the daylight guarantee has become conditional
    // on arithmetic somewhere and the network is free to seal itself.
    CHECK_EQ(daylit, opened);
    CHECK_EQ(reachesNode, opened);
}

VXC_TEST(cave_entrance_is_a_cavity_with_a_roof_not_a_bore) {
    // THE SHAPE CHANGE ITSELF, measured rather than eyeballed. The owner judges
    // screenshots; a test still has to be able to say the geometry is the one
    // that was designed.
    //
    // Two properties separate a v25 cavity from a v24 cylinder, and a cylinder
    // has neither at any column:
    //   * ROOFED VOID -- the entrance carves rock with intact ground above it
    //     (shaftDepthMinMm > 0). On flat land this set is a doline's
    //     overhanging lip; on a slope it is the whole length of a mouth.
    //   * A LEVEL FLOOR -- the cavity is anchored at absolute z, so across the
    //     footprint the deepest carved voxel sits at ONE elevation instead of
    //     draping under the terrain. That is what a mob stands on.
    int64_t ei = 0, ej = 0;
    CHECK(firstOpenEntranceNode(0, 0, 64, ei, ej));
    const CaveNode n = caveNode(kSeed, ei, ej);
    const int64_t nvx = floorDiv(n.xMm, int64_t(kVoxelSizeMm));
    const int64_t nvy = floorDiv(n.yMm, int64_t(kVoxelSizeMm));

    int64_t footprint = 0, roofed = 0, opened = 0;
    int64_t floorZMin = 1ll << 40, floorZMax = -(1ll << 40);
    for (int64_t dy = -160; dy <= 160; ++dy)
        for (int64_t dx = -160; dx <= 160; ++dx) {
            const CaveColumn c =
                caveColumnFor(kSeed, nvx + dx, nvy + dy, kFlatSurfaceMm, kFlatSurfaceAt);
            if (c.shaftMarginSq <= 0) continue;
            ++footprint;
            if (c.shaftDepthMinMm > 0) ++roofed; else ++opened;
            // Deepest carved voxel of the ENTRANCE, as an absolute elevation.
            // Throat columns bore past the cavity floor to the node, so they
            // are excluded from the level-floor statement -- not from the
            // footprint, which they are legitimately part of.
            if (c.shaftDepthMaxMm < kCaveNodeDepthMinMm) {
                const int64_t deepestZMm = int64_t(kFlatSurfaceMm) - c.shaftDepthMaxMm;
                if (deepestZMm < floorZMin) floorZMin = deepestZMm;
                if (deepestZMm > floorZMax) floorZMax = deepestZMm;
            }
        }
    std::printf("    [caves] entrance at lattice (%lld,%lld): %lld footprint columns, "
                "%lld roofed / %lld open to the sky; cavity floor spans %lld mm\n",
                (long long)ei, (long long)ej, (long long)footprint, (long long)roofed,
                (long long)opened, (long long)(floorZMax - floorZMin));
    CHECK(footprint > 0);
    // A bore has no roofed void at all, anywhere. The lip is most of the
    // footprint, which is what makes the visible hole much smaller than the
    // chamber under it.
    CHECK(roofed > 0);
    CHECK(roofed > opened);
    // ...and it is still an entrance, not a sealed chamber.
    CHECK(opened > 0);
    // The floor is LEVEL to within one voxel: it is one absolute-z plane, so
    // the only spread allowed is that plane's voxel quantisation.
    CHECK(floorZMax - floorZMin <= kVoxelSizeMm);
}

VXC_TEST(cave_entrance_daylights_sideways_on_real_relief) {
    // THE MOUNTAINSIDE MOUTH -- the owner's Q6 item that v24 could not produce
    // at all. caves.h's original header claimed tunnels "daylight sideways on
    // steep slopes for free"; vxc_caveprobe measured that claim at the
    // grassland site and found the sideways-mouth count EQUAL to the perforated
    // shaft count, i.e. zero mouths that were not simply a hole seen from
    // below. A tunnel is >= 6 m under its own column, so for a neighbour a
    // metre away to lie below it the ground has to fall 6 m in 1 m.
    //
    // v25 gets mouths out of the SAME construct as dolines, by clipping the
    // cavity's roof against the real ground: where the ground falls away the
    // roof becomes the hillside, and the chamber opens through it.
    //
    // THE SIGNATURE, AND THE ONE THAT DID NOT WORK. This test first asserted "a
    // roofed entrance void with cover thinner than the 6 m roof clamp", which
    // looks like a hillside signature and is not one: near the rim the lens roof
    // pinches to nothing, so the cover there is just the drawn floor depth,
    // [5, 9) m, which is under 6 m for more than half of all sites on DEAD FLAT
    // ground. It measures the lens, not the hill, and it passed vacuously.
    //
    // What actually separates the two is WHICH SURFACE CLIPPED THE ROOF, and
    // the open set's shape is the readout: the cavity is open to the sky exactly
    // where the ground clipped it. On flat ground that is a compact disc around
    // the axis, strictly interior to the footprint. On falling ground it is
    // pushed off-axis and reaches the footprint RIM, because out there the hill
    // has cut the roof away completely -- which is a horizontal mouth. So this
    // asserts an open column in the OUTER FIFTH of the footprint, which a bore
    // cannot produce and a flat-ground doline does not.
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    int64_t ei = 0, ej = 0;
    CHECK(firstOpenEntranceNode(0, 0, 64, ei, ej));
    const CaveNode n = caveNode(kSeed, ei, ej);
    const int64_t nvx = floorDiv(n.xMm, int64_t(kVoxelSizeMm));
    const int64_t nvy = floorDiv(n.yMm, int64_t(kVoxelSizeMm));

    int64_t footprint = 0, roofed = 0, rimOpen = 0, floorSpots = 0, maxRSq = 0;
    int32_t openMinSurfMm = INT32_MAX, openMaxSurfMm = INT32_MIN;
    // Two passes -- "the outer fifth" is meaningless before the footprint
    // radius is known, and the radius is a per-site hash draw.
    for (int64_t dy = -160; dy <= 160; dy += 2)
        for (int64_t dx = -160; dx <= 160; dx += 2) {
            const ColumnSample col = amp.column(nvx + dx, nvy + dy);
            if (col.cave.shaftMarginSq <= 0) continue;
            ++footprint;
            if (dx * dx + dy * dy > maxRSq) maxRSq = dx * dx + dy * dy;
            if (col.cave.shaftDepthMinMm > 0) {
                ++roofed;
                continue;
            }
            if (col.surfaceMm < openMinSurfMm) openMinSurfMm = col.surfaceMm;
            if (col.surfaceMm > openMaxSurfMm) openMaxSurfMm = col.surfaceMm;
            // Somewhere to stand inside it: the solid voxel under the void.
            const int64_t floorVz =
                floorDiv(int64_t(col.surfaceMm) - col.cave.shaftDepthMaxMm - kVoxelSizeMm / 2,
                         int64_t(kVoxelSizeMm)) -
                1;
            if (Amplifier::materialAt(col, floorVz) != MAT_AIR) ++floorSpots;
        }
    for (int64_t dy = -160; dy <= 160 && maxRSq > 0; dy += 2)
        for (int64_t dx = -160; dx <= 160; dx += 2) {
            const ColumnSample col = amp.column(nvx + dx, nvy + dy);
            if (col.cave.shaftMarginSq <= 0 || col.cave.shaftDepthMinMm > 0) continue;
            if ((dx * dx + dy * dy) * 25 >= maxRSq * 16) ++rimOpen; // r >= 0.8 R
        }
    std::printf("    [caves] entrance on real terrain: %lld footprint columns (r=%.1f m), "
                "%lld roofed, %lld open in the outer fifth (the hillside mouth), ground across "
                "the opening %.1f..%.1f m, %lld standable floor spots\n",
                (long long)footprint, std::sqrt(double(maxRSq)) * kVoxelSizeMm / 1000.0,
                (long long)roofed, (long long)rimOpen,
                openMinSurfMm == INT32_MAX ? 0.0 : openMinSurfMm / 1000.0,
                openMaxSurfMm == INT32_MIN ? 0.0 : openMaxSurfMm / 1000.0, (long long)floorSpots);
    CHECK(footprint > 0);
    CHECK(roofed > 0);
    // The mouth itself: the cavity daylights at its own rim, which only falling
    // ground can do. A bore cannot, and a flat-ground doline does not.
    CHECK(rimOpen > 0);
    // ...and the ground really is falling across the opening, so the line above
    // is about the hill and not about an unlucky rim column.
    CHECK(openMaxSurfMm - openMinSurfMm > 2000);
    // Mobs (plan 5.6): an entrance a player can walk into needs ground under it.
    CHECK(floorSpots > 0);
}

// --- gameplay coupling: cave air is ordinary air ----------------------------

VXC_TEST(cave_air_is_plain_air_reachable_from_the_surface) {
    // M6 pathfinding, M5 digging and the water CA all key off MAT_AIR, so the
    // only thing the cave pass has to do to plug into them is produce MAT_AIR
    // â€” which this asserts â€” plus actually have openings to the outside. A
    // "mouth" here is a carved voxel face-adjacent to above-surface air, which
    // is what a hillside daylighting looks like in voxel terms.
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    GeneratedWorld<8> gen(amp);

    size_t shaftColumns = 0, carved = 0, sampled = 0;
    for (int64_t x = -1024; x <= 1024; x += 3) {
        for (int64_t y = -1024; y <= 1024; y += 3) {
            const ColumnSample col = amp.column(x, y);
            ++sampled;
            if (col.cave.shaftMarginSq > 0) ++shaftColumns;
            if (col.cave.count == 0) continue;
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            for (int64_t vz = topVz - 60; vz > topVz - 400; --vz) {
                if (Amplifier::stratigraphyAt(col, vz) == MAT_AIR) continue;
                if (Amplifier::materialAt(col, vz) != MAT_AIR) continue;
                ++carved;
            }
        }
    }
    CHECK(carved > 0);
    CHECK(shaftColumns > 0); // entrances exist in a 204.8 m square of real terrain
    std::printf("    [caves] openings: %zu of %zu sampled surface columns fall inside a "
                "sinkhole shaft; %zu cave voxels sampled below\n",
                shaftColumns, sampled, carved);

    // And the same air shows up through the brick/mesh path, not just the
    // pointwise query.
    const auto grid = gen.columns(0, 0);
    int32_t bzMin, bzMax;
    gen.surfaceBrickRange(grid, bzMin, bzMax);
    const Brick<8> deep = gen.makeBrick(BrickKey{0, 0, bzMin - 20}, grid);
    for (int z = 0; z < 8; ++z)
        for (int yy = 0; yy < 8; ++yy)
            for (int xx = 0; xx < 8; ++xx)
                CHECK_EQ(deep.get(xx, yy, z),
                         gen.materialAt(xx, yy, int64_t(bzMin - 20) * 8 + z));
}

// --- volume budget ----------------------------------------------------------

VXC_TEST(cave_volume_fraction_is_cave_like_not_sponge_like) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    size_t solidBand = 0, carvedBand = 0;
    for (int64_t x = -512; x <= 512; x += 5)
        for (int64_t y = -512; y <= 512; y += 7) {
            const ColumnSample col = amp.column(x, y);
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            // The band the cave pass is allowed to touch: 6 m .. 40 m down.
            for (int64_t vz = topVz - 60; vz > topVz - 400; --vz) {
                if (Amplifier::stratigraphyAt(col, vz) == MAT_AIR) continue;
                ++solidBand;
                if (Amplifier::materialAt(col, vz) == MAT_AIR) ++carvedBand;
            }
        }
    CHECK(solidBand > 0);
    const double frac = double(carvedBand) / double(solidBand);
    std::printf("    [caves] volume: %.3f%% of the 6-40 m subsurface band is cave air\n",
                100.0 * frac);
    CHECK(frac > 0.001); // caves exist
    CHECK(frac < 0.15);  // ...and the ground is still ground
}

// --- per-family census (W1c of docs/underground-system-plan.md) -------------
//
// The three tests below exist because every number the underground redesign
// will be judged on -- how much of the void is crevice, how many entrances
// there are, whether there is anywhere for a mob to stand -- was previously
// unmeasurable: caves.h flattens tunnels and crevices into one untagged
// segs[] array, and nothing anywhere counted caverns separately from tunnels.
// voxelcore/cave_families.h recovers the labels by differencing the SHIPPING
// predicate over controlled lattice variants (see that header). vxc_caveprobe
// reports the same statistics over a chosen world region with pictures; these
// are the cheap ctest gates on the same quantities, so a retune that quietly
// deletes a whole family or seals the network fails the build rather than
// waiting to be noticed in a screenshot.
//
// The thresholds are deliberately wide. They are "this family still exists and
// has not run away", not a pin of today's tuning -- W2 through W8 are all
// EXPECTED to move these numbers, and a gate that has to be edited on every
// commit is a gate nobody reads.

VXC_TEST(cave_family_attribution_is_exact) {
    // The load-bearing claim of cave_families.h: the attribution is not an
    // approximation. At every voxel, (tunnel | crevice | shaft) must equal
    // caveCarveAt's own answer, and the recomputed lattice must reproduce the
    // ColumnSample's memoised cave column. If a future change to caves.h adds
    // a fourth thing to segs[], or makes the tunnel emission read crevHash,
    // this fails here rather than silently skewing every census that uses it.
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    int64_t voxels = 0, mismatches = 0, columns = 0, memoMismatches = 0;
    for (int64_t x = -512; x <= 512; x += 7)
        for (int64_t y = -512; y <= 512; y += 11) {
            const ColumnSample col = amp.column(x, y);
            const CaveColumnVariants cv = caveColumnVariantsFor(kSeed, x, y, col.surfaceMm, amp.surfaceAtFn());
            ++columns;
            if (cv.full.count != col.cave.count ||
                cv.full.shaftMarginSq != col.cave.shaftMarginSq ||
                cv.full.shaftDepthMaxMm != col.cave.shaftDepthMaxMm)
                ++memoMismatches;
            for (int32_t s = 0; s < cv.full.count && s < col.cave.count; ++s)
                if (cv.full.segs[s].marginSq != col.cave.segs[s].marginSq ||
                    cv.full.segs[s].depthMm != col.cave.segs[s].depthMm)
                    ++memoMismatches;

            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            for (int64_t vz = topVz; vz > topVz - 450; --vz) {
                if (Amplifier::stratigraphyAt(col, vz) == MAT_AIR) continue;
                ++voxels;
                bool truth = false;
                const uint32_t m = caveFamilyMaskAt(cv, col.cavern, col.surfaceMm,
                                                    col.bedrockDepthMm, vz, truth);
                if (caveFamilyMaskIsCavePass(m) != truth) ++mismatches;
            }
        }
    std::printf("    [caves] attribution: %lld voxels over %lld columns, %lld family "
                "mismatches, %lld lattice/memo mismatches\n",
                (long long)voxels, (long long)columns, (long long)mismatches,
                (long long)memoMismatches);
    CHECK(voxels > 0);
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(memoMismatches, 0);
}

VXC_TEST(cave_per_family_volume_and_entrance_census) {
    // FOOTPRINT, not stride, is what decides whether this test can see an
    // entrance. Sinkhole candidate nodes are 102.4 m apart and 1-in-4 gated,
    // so a +/-51.2 m box (the footprint most of this file uses) usually
    // contains NO open shaft and reports "zero entrances exist" -- a property
    // of the box, not of the world. It did exactly that when this test was
    // first written. +/-102.4 m with a coarser stride costs the same and
    // covers a full shaft period on both axes.
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    int64_t solid = 0, columns = 0, colsWithAny = 0, perforated = 0;
    int64_t famVox[kCaveFamilyCount] = {0, 0, 0, 0};
    int64_t famCols[kCaveFamilyCount] = {0, 0, 0, 0};
    for (int64_t x = -1024; x <= 1024; x += 11)
        for (int64_t y = -1024; y <= 1024; y += 13) {
            const ColumnSample col = amp.column(x, y);
            const CaveColumnVariants cv = caveColumnVariantsFor(kSeed, x, y, col.surfaceMm, amp.surfaceAtFn());
            ++columns;
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            uint32_t colMask = 0;
            bool topCarved = false;
            for (int64_t vz = topVz; vz > topVz - 450; --vz) {
                if (Amplifier::stratigraphyAt(col, vz) == MAT_AIR) continue;
                ++solid;
                bool truth = false;
                const uint32_t m = caveFamilyMaskAt(cv, col.cavern, col.surfaceMm,
                                                    col.bedrockDepthMm, vz, truth);
                if (!m) continue;
                if (vz == topVz) topCarved = true;
                colMask |= m;
                for (uint32_t f = 0; f < kCaveFamilyCount; ++f)
                    if (m & (1u << f)) ++famVox[f];
            }
            if (colMask) ++colsWithAny;
            for (uint32_t f = 0; f < kCaveFamilyCount; ++f)
                if (colMask & (1u << f)) ++famCols[f];
            if (topCarved) ++perforated;
        }
    CHECK(solid > 0);
    CHECK(columns > 0);
    static const char* kName[kCaveFamilyCount] = {"tunnel", "crevice", "shaft", "cavern"};
    for (uint32_t f = 0; f < kCaveFamilyCount; ++f)
        std::printf("    [caves] family %-8s %10lld voxels (%.4f%% of the solid band), "
                    "%lld columns (%.3f%%)\n",
                    kName[f], (long long)famVox[f], 100.0 * double(famVox[f]) / double(solid),
                    (long long)famCols[f], 100.0 * double(famCols[f]) / double(columns));
    std::printf("    [caves] entrances: %lld of %lld sampled surface columns are perforated "
                "(%.4f%%); %lld columns (%.2f%%) have some cave beneath\n",
                (long long)perforated, (long long)columns,
                100.0 * double(perforated) / double(columns), (long long)colsWithAny,
                100.0 * double(colsWithAny) / double(columns));

    // Every family the generator set claims to have must actually be present.
    // "Crevices fire" was previously only evidenced indirectly, by a segment
    // count exceeding four; this counts their voxels.
    CHECK(famVox[CAVE_FAM_TUNNEL] > 0);
    CHECK(famVox[CAVE_FAM_CREVICE] > 0);
    CHECK(famVox[CAVE_FAM_SHAFT] > 0);
    // Tunnels dominate the void by construction (crevices are thin slabs
    // riding tunnels, shafts are ~1.4 m bores).
    CHECK(famVox[CAVE_FAM_TUNNEL] > famVox[CAVE_FAM_CREVICE]);
    CHECK(famVox[CAVE_FAM_SHAFT] < famVox[CAVE_FAM_TUNNEL]);
    // The surface is not a colander. Same statement as the roof test's
    // shaft-column bound, but stated on CARVED surface voxels rather than on
    // columns that merely fall inside a shaft radius.
    CHECK(perforated * 200 < columns);
    // ...and it is not sealed either: entrances exist in this footprint.
    CHECK(perforated > 0);
}

VXC_TEST(cave_floor_area_and_headroom_budget) {
    // Plan 5.6: mobs are coming, and they are a client of every generator.
    // "Navigable floor and headroom" is currently an assertion about tube
    // diameters; this measures the thing that actually matters -- solid ground
    // with continuous walkable air above it, per family -- so a future spawn
    // system inherits a measured surface budget instead of a guess, and so a
    // passage rework that quietly leaves nowhere to stand is caught.
    constexpr int64_t kHeadVox = 18; // 1.8 m of clearance
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);

    int64_t columns = 0, floorSpots[kCaveFamilyCount] = {0, 0, 0, 0};
    int64_t floorCols = 0, headroomSumVox = 0;
    std::vector<uint32_t> fam(451, 0);
    std::vector<uint8_t> air(451, 0);
    for (int64_t x = -1024; x <= 1024; x += 11)
        for (int64_t y = -1024; y <= 1024; y += 13) {
            const ColumnSample col = amp.column(x, y);
            const CaveColumnVariants cv = caveColumnVariantsFor(kSeed, x, y, col.surfaceMm, amp.surfaceAtFn());
            ++columns;
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            for (int64_t k = 0; k <= 450; ++k) {
                const int64_t vz = topVz - k;
                if (Amplifier::stratigraphyAt(col, vz) == MAT_AIR) {
                    fam[size_t(k)] = 0;
                    air[size_t(k)] = 1;
                    continue;
                }
                bool truth = false;
                const uint32_t m = caveFamilyMaskAt(cv, col.cavern, col.surfaceMm,
                                                    col.bedrockDepthMm, vz, truth);
                fam[size_t(k)] = m;
                air[size_t(k)] = m ? 1 : 0;
            }
            bool anyHere = false;
            for (int64_t k = 1; k <= 450; ++k) {
                if (air[size_t(k)]) continue;             // not solid ground
                if (!fam[size_t(k - 1)]) continue;        // the air above is not cave air
                int64_t clear = 0;
                for (; clear < kHeadVox; ++clear) {
                    const int64_t kk = k - 1 - clear;
                    if (kk < 0) break; // ran out of the scanned band: open to the sky
                    if (!air[size_t(kk)]) break;
                }
                if (clear < kHeadVox) continue;
                const int32_t f = caveDominantFamily(fam[size_t(k - 1)]);
                if (f < 0) continue;
                ++floorSpots[f];
                headroomSumVox += clear;
                anyHere = true;
            }
            if (anyHere) ++floorCols;
        }

    int64_t total = 0;
    for (uint32_t f = 0; f < kCaveFamilyCount; ++f) total += floorSpots[f];
    static const char* kName[kCaveFamilyCount] = {"tunnel", "crevice", "shaft", "cavern"};
    for (uint32_t f = 0; f < kCaveFamilyCount; ++f)
        std::printf("    [caves] standable floor, %-8s %lld spots\n", kName[f],
                    (long long)floorSpots[f]);
    std::printf("    [caves] standable floor: %lld spots over %lld sampled columns "
                "(%.2f%% of columns), mean clearance %.2f m at a %.1f m requirement\n",
                (long long)total, (long long)columns, 100.0 * double(floorCols) / double(columns),
                total ? double(headroomSumVox) * kVoxelSizeMm / 1000.0 / double(total) : 0.0,
                double(kHeadVox * kVoxelSizeMm) / 1000.0);

    CHECK(columns > 0);
    // There is somewhere to stand, and it is a routine feature rather than a
    // curiosity: a walkable tunnel floor under at least 1% of sampled columns.
    CHECK(total > 0);
    CHECK(floorSpots[CAVE_FAM_TUNNEL] > 0);
    CHECK(floorCols * 100 > columns);
}
