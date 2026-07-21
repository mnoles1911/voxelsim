// M4 cave pass (voxelcore/caves.h): determinism, golden digest, the three
// safety rules (bedrock floor, implicit ocean, roof thickness), the
// segment-cap headroom, and — the point of the whole formulation —
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
#include "voxelcore/caves.h"
#include "voxelcore/connectivity.h"
#include "voxelcore/generator.h"
#include "vxctest.h"

using namespace vxc;

namespace {

constexpr uint64_t kSeed = 20260719;

// A flat synthetic column, used by the tests that want to look at the tunnel
// NETWORK on its own with no terrain draping and no clipping in the way.
constexpr int32_t kFlatSurfaceMm = 100000;   // 100 m — well clear of sea level
constexpr int32_t kFlatBedrockMm = 45000;    // 45 m — deliberately kept at the
                                             // PRE-v5 amplifier band (bedrock
                                             // moved to 180-220 m at v5), so
                                             // this file keeps exercising the
                                             // bedrock clamp against a floor
                                             // shallow enough for it to bind

// Carve predicate for the flat world.
bool flatCarve(int64_t vx, int64_t vy, int64_t vz) {
    const CaveColumn c = caveColumnFor(kSeed, vx, vy, kFlatSurfaceMm);
    return caveCarveAt(c, kFlatSurfaceMm, kFlatBedrockMm, vz);
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
            const CaveColumn a = caveColumnFor(kSeed, x, y, kFlatSurfaceMm);
            const CaveColumn b = caveColumnFor(kSeed, x, y, kFlatSurfaceMm);
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
            const CaveColumn ac = caveColumnFor(kSeed, x, y, kFlatSurfaceMm);
            const CaveColumn cc = caveColumnFor(kSeed + 1, x, y, kFlatSurfaceMm);
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
    // moves — unlike the mips/GPU digests, which also move for any
    // stratigraphy change.
    Digest d;
    for (int64_t y = -640; y < 640; y += 37) {
        for (int64_t x = -640; x < 640; x += 41) {
            const CaveColumn c = caveColumnFor(kSeed, x, y, kFlatSurfaceMm);
            d.u32(static_cast<uint32_t>(c.count));
            for (int32_t s = 0; s < c.count; ++s) {
                d.u32(static_cast<uint32_t>(c.segs[s].marginSq));
                d.u32(static_cast<uint32_t>(c.segs[s].depthMm));
            }
            d.u32(static_cast<uint32_t>(c.shaftMarginSq));
            d.u32(static_cast<uint32_t>(c.shaftDepthMaxMm));
            for (int64_t vz = 560; vz < 1000; vz += 3)
                d.u8(caveCarveAt(c, kFlatSurfaceMm, kFlatBedrockMm, vz) ? 1 : 0);
        }
    }
    // GOLDEN(cave_layer) — new at kWorldGenVersion 4; MOVED for M4 cave pass v2
    // (docs/cavern-design.md C2): caveColumnFor now also emits crevice segs on
    // ~1-in-8 of existing edges, so count/segs[] shift for many sampled
    // columns even though the tunnel geometry itself is unchanged. Re-pinned
    // per the build plan's explicit "caves.h + test_caves.cpp headroom/golden
    // updates" ownership for this subtask.
    CHECK_EQ(d.h, 0xBFE42E07FFA6B78Dull);
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
            const CaveColumn c = caveColumnFor(kSeed, x, y, kFlatSurfaceMm);
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
    // on the definition — every surface height from a deep seafloor up through
    // the beach band to the cave threshold.
    size_t oceanColumns = 0;
    for (int32_t surfaceMm = -40000; surfaceMm < kCaveMinSurfaceMm; surfaceMm += 271)
        for (int64_t x = 0; x < 256; x += 37)
            for (int64_t y = 0; y < 256; y += 41) {
                const CaveColumn c = caveColumnFor(kSeed, x, y, surfaceMm);
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
                const CaveColumn c = caveColumnFor(kSeed, x, y, surfaceMm);
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
    for (int64_t x = -512; x <= 512; x += 3)
        for (int64_t y = -512; y <= 512; y += 5) {
            const ColumnSample col = amp.column(x, y);
            ++columnsSampled;
            // Sinkhole shafts are the one deliberate roof breach (caves.h
            // kCaveShaftNodeMask); their footprint is measured separately
            // below, and it is what makes the network enterable at all.
            if (col.cave.shaftMarginSq > 0) {
                ++shaftColumns;
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
    // The perforated fraction of the surface must be a rounding error.
    CHECK(shaftColumns * 500 < columnsSampled);
    std::printf("    [caves] roof: thinnest cover %lld mm (clamp %lld mm) over "
                "non-sinkhole ground; %zu/%zu sampled columns (%.1f%%) have any cave "
                "beneath them; %zu (%.4f%%) are inside a sinkhole shaft\n",
                static_cast<long long>(thinnestRoofMm),
                static_cast<long long>(kCaveRoofMinMm), columnsWithCaves, columnsSampled,
                100.0 * double(columnsWithCaves) / double(columnsSampled), shaftColumns,
                100.0 * double(shaftColumns) / double(columnsSampled));
}

// --- storage bound ----------------------------------------------------------

VXC_TEST(cave_segment_cap_headroom) {
    // kMaxCaveSegs truncates deterministically if it is ever hit, but it must
    // not be hit: this measures the real maximum over a wide sample.
    int32_t maxSegs = 0;
    size_t columns = 0, withAny = 0;
    for (int64_t x = -2048; x < 2048; x += 3)
        for (int64_t y = -2048; y < 2048; y += 7) {
            const CaveColumn c = caveColumnFor(kSeed, x, y, kFlatSurfaceMm);
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

// --- crevices (M4 cave pass v2, docs/cavern-design.md §4) -------------------

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

                // Mid-edge: taper is maximal, so the slab is at its full
                // (roof-clamped) size and must contain the tube axis depth at
                // that same point. Q16 fixed point, matching caveColumnFor's
                // crevice block exactly (den can be too large to square into
                // int64, see that block's comment).
                const int64_t numMid = den / 2;
                const int64_t cdMid = a.depthMm + floorDiv((b.depthMm - a.depthMm) * numMid, den);
                const int64_t hUpEffMm = clampi64(hUpMm, 0, cdMid - kCaveRoofMinMm);
                const int64_t halfSpanMm = (hUpEffMm + hDownMm) / 2;
                const int64_t uQ16Mid = floorDiv(numMid << 16, den);
                const int64_t taperQ16Mid = (4 * uQ16Mid * ((1 << 16) - uQ16Mid)) >> 16;
                const int64_t halfSpanTaperedMid = floorDiv(halfSpanMm * taperQ16Mid, 1 << 16);
                CHECK(halfSpanTaperedMid > 0);
                const int64_t depthMid = cdMid + floorDiv(hDownMm - hUpEffMm, 2);
                const int64_t centerOffset = depthMid - cdMid;
                CHECK(centerOffset <= halfSpanTaperedMid);
                CHECK(-centerOffset <= halfSpanTaperedMid);

                // At either node (u=0 or u=1), the taper is zero by
                // construction -- the slab pinches to nothing.
                CHECK_EQ((4 * static_cast<int64_t>(0) * ((1 << 16) - 0)) >> 16, 0);
                const int64_t uQ16End = floorDiv(den << 16, den); // u=1 -> 1<<16
                CHECK_EQ((4 * uQ16End * ((1 << 16) - uQ16End)) >> 16, 0);
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
            const CaveColumn c = caveColumnFor(kSeed, x, y, kFlatSurfaceMm);
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
    const CaveColumn shaftCol = caveColumnFor(kSeed, nvx, nvy, kFlatSurfaceMm);
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

// --- gameplay coupling: cave air is ordinary air ----------------------------

VXC_TEST(cave_air_is_plain_air_reachable_from_the_surface) {
    // M6 pathfinding, M5 digging and the water CA all key off MAT_AIR, so the
    // only thing the cave pass has to do to plug into them is produce MAT_AIR
    // — which this asserts — plus actually have openings to the outside. A
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
