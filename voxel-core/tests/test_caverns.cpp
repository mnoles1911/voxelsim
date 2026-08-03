// M4 cave pass v2 (voxelcore/caverns.h): determinism, golden digest,
// connectivity evidence (structural claim verified by an actual flood fill,
// not assumed), the safety clamps (bedrock/sea-level/roof â€” the latter
// measured on genuinely varied, sloped real terrain per Matt's post-design
// direction, not just flat ground), the segment-cap headroom, flood-level
// properties, and the measured per-column/per-voxel cost budget.
//
// Mirrors the discipline in test_caves.cpp throughout, including the
// decimated-sample-grid flood-fill technique (every 4th voxel) for the
// connectivity tests.

#include <chrono>
#include <cstdio>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/caverns.h"
#include "voxelcore/connectivity.h"
#include "vxctest.h"

using namespace vxc;

namespace {

constexpr uint64_t kSeed = 20260719;

// A flat synthetic column/surface, used by the tests that want to look at
// the cavern SYSTEM on its own with no terrain draping in the way.
constexpr int32_t kFlatSurfaceMm = 100000; // 100 m â€” well clear of sea level
constexpr int32_t kFlatBedrockMm = 250000; // 250 m â€” comfortably past the post-
                                            // design 200 m bedrock move; this
                                            // file never assumes a specific
                                            // value, it is just a plausible
                                            // one for exercising the (always
                                            // runtime-parameterised) clamp.

int32_t flatSurfaceAt(int64_t, int64_t) { return kFlatSurfaceMm; }

CavernColumn flatCavernColumnFor(int64_t vx, int64_t vy) {
    return cavernColumnFor(kSeed, vx, vy, kFlatSurfaceMm, flatSurfaceAt);
}

bool flatCavernCarve(int64_t vx, int64_t vy, int64_t vz) {
    const CavernColumn c = flatCavernColumnFor(vx, vy);
    return cavernCarveAt(c, kFlatSurfaceMm, kFlatBedrockMm, vz);
}

bool flatEitherCarve(int64_t vx, int64_t vy, int64_t vz) {
    if (flatCavernCarve(vx, vy, vz)) return true;
    const CaveColumn tc = caveColumnFor(kSeed, vx, vy, kFlatSurfaceMm, flatSurfaceAt);
    return caveCarveAt(tc, kFlatSurfaceMm, kFlatBedrockMm, vz);
}

// A discovered, valid cavern site (flat terrain), used by several tests below
// so each doesn't have to repeat the discovery scan.
struct FoundSite {
    bool found = false;
    int64_t fi = 0, fj = 0;
    CaveNode node;
    CavernSite site;
};

FoundSite findFirstValidFlatSite(int64_t maxCorner = 400) {
    FoundSite out;
    for (int64_t fj = 0; fj <= maxCorner && !out.found; fj += kCavernCoarseLatticeRatio) {
        for (int64_t fi = 0; fi <= maxCorner && !out.found; fi += kCavernCoarseLatticeRatio) {
            if (!cavernSiteGateOpen(kSeed, fi, fj)) continue;
            const CaveNode node = caveNode(kSeed, fi, fj);
            if (!cavernDepthIsSafe(node.depthMm)) continue;
            const CavernSite site = cavernSiteFor(kSeed, fi, fj, node, flatSurfaceAt);
            if (!site.valid) continue;
            out.found = true;
            out.fi = fi;
            out.fj = fj;
            out.node = node;
            out.site = site;
        }
    }
    return out;
}

struct ComponentStats {
    int32_t count = 0;
    size_t total = 0;
    size_t largest = 0;
};

ComponentStats summarize(const ConnectivityResult& r) {
    ComponentStats s;
    s.count = r.componentCount;
    for (const Component& c : r.components) {
        s.total += c.size();
        if (c.size() > s.largest) s.largest = c.size();
    }
    return s;
}

} // namespace

// --- determinism -------------------------------------------------------------

VXC_TEST(cavern_column_is_deterministic) {
    for (int64_t x = -40000; x <= 40000; x += 613)
        for (int64_t y = -40000; y <= 40000; y += 419) {
            const CavernColumn a = flatCavernColumnFor(x, y);
            const CavernColumn b = flatCavernColumnFor(x, y);
            CHECK_EQ(a.count, b.count);
            CHECK_EQ(a.floodZMm, b.floodZMm);
            for (int32_t s = 0; s < a.count; ++s) {
                CHECK_EQ(a.segs[s].marginSq, b.segs[s].marginSq);
                CHECK_EQ(a.segs[s].zCenterMm, b.segs[s].zCenterMm);
                CHECK_EQ(a.segs[s].zFloorMm, b.segs[s].zFloorMm);
            }
        }

    // A different seed must produce different geometry (channel separation
    // is real, not accidentally seed-independent).
    bool differs = false;
    for (int64_t x = -40000; x <= 40000 && !differs; x += 613) {
        for (int64_t y = -40000; y <= 40000 && !differs; y += 419) {
            const CavernColumn a = cavernColumnFor(kSeed, x, y, kFlatSurfaceMm, flatSurfaceAt);
            const CavernColumn b = cavernColumnFor(kSeed + 1, x, y, kFlatSurfaceMm, flatSurfaceAt);
            if (a.count != b.count || a.floodZMm != b.floodZMm) differs = true;
            else
                for (int32_t s = 0; s < a.count; ++s)
                    if (a.segs[s].zCenterMm != b.segs[s].zCenterMm) differs = true;
        }
    }
    CHECK(differs);
}

// --- golden digest ------------------------------------------------------------

VXC_TEST(cavern_golden_digest) {
    Digest d;
    for (int64_t y = -6000; y < 6000; y += 37) {
        for (int64_t x = -6000; x < 6000; x += 41) {
            const CavernColumn c = flatCavernColumnFor(x, y);
            d.u32(static_cast<uint32_t>(c.count));
            for (int32_t s = 0; s < c.count; ++s) {
                d.u32(static_cast<uint32_t>(c.segs[s].marginSq));
                d.u32(static_cast<uint32_t>(c.segs[s].zCenterMm));
                d.u32(static_cast<uint32_t>(c.segs[s].zFloorMm));
            }
            d.u32(static_cast<uint32_t>(c.floodZMm));
        }
    }
    // GOLDEN(cavern_layer) â€” new for the M4 cave pass v2 cavern system.
    CHECK_EQ(d.h, 0xFB45CBD3F95E65C4ull);
}

// --- chain geometry: consecutive rooms overlap for real hashed sites --------

VXC_TEST(cavern_chain_rooms_overlap_pairwise_for_real_sites) {
    // The static_asserts in caverns.h prove this for every possible hash
    // draw using worst-case constants; this measures it directly against
    // real hashed sites as a second, independent check (mirroring the
    // discipline of measuring rather than only trusting a compile-time
    // argument, same spirit as caves.h's own connectivity tests).
    int64_t sitesChecked = 0;
    int64_t maxSpanMm = 0;
    for (int64_t fj = 0; fj <= 800 && sitesChecked < 60; fj += kCavernCoarseLatticeRatio) {
        for (int64_t fi = 0; fi <= 800 && sitesChecked < 60; fi += kCavernCoarseLatticeRatio) {
            if (!cavernSiteGateOpen(kSeed, fi, fj)) continue;
            const CaveNode node = caveNode(kSeed, fi, fj);
            if (!cavernDepthIsSafe(node.depthMm)) continue;
            const CavernSite site = cavernSiteFor(kSeed, fi, fj, node, flatSurfaceAt);
            if (!site.valid) continue;
            ++sitesChecked;

            int64_t lowestMm = site.anchorZMm + site.children[0].rzMm;
            for (int32_t c = 0; c < kCavernChildCount; ++c) {
                const CavernChild& ch = site.children[c];
                CHECK(ch.rxyMm >= kCavernRxyMinMm);
                CHECK(ch.rxyMm < kCavernRxyMinMm + kCavernRxySpanMm);
                CHECK(ch.rzMm > 0);
                if (c > 0) {
                    const CavernChild& prev = site.children[c - 1];
                    // Coaxial 1D interval overlap, exact (see caverns.h "why
                    // coaxial"): |dz| < rz[c-1] + rz[c].
                    const int64_t dz = prev.zMm - ch.zMm;
                    CHECK(dz > 0); // strictly descending
                    CHECK(dz < prev.rzMm + ch.rzMm);
                }
                const int64_t bottom = ch.zMm - ch.rzMm;
                if (bottom < lowestMm) lowestMm = bottom;
            }
            const int64_t topMm = site.anchorZMm + site.children[0].rzMm;
            const int64_t spanMm = topMm - lowestMm;
            if (spanMm > maxSpanMm) maxSpanMm = spanMm;
        }
    }
    CHECK(sitesChecked > 0);
    std::printf("    [caverns] chain geometry: %lld real sites checked, all pairwise "
                "overlaps hold; widest observed vertical span %.1f m\n",
                static_cast<long long>(sitesChecked), static_cast<double>(maxSpanMm) / 1000.0);
    // The whole point of the chain is genuine multi-storey depth: this
    // should be well beyond a single 12 m-tall room.
    CHECK(maxSpanMm > 30000);
}

// --- connectivity: structural claim, flat terrain ---------------------------

VXC_TEST(cavern_anchor_point_is_carved_by_both_cavern_and_tunnel_systems) {
    // The direct structural claim: a cavern's anchor sits exactly on a point
    // the tunnel network already carves (same node, same depth), so both
    // caveCarveAt and cavernCarveAt must agree that voxel is air, for EVERY
    // valid site â€” not just the lucky first one.
    int64_t sitesChecked = 0;
    for (int64_t fj = 0; fj <= 800 && sitesChecked < 40; fj += kCavernCoarseLatticeRatio) {
        for (int64_t fi = 0; fi <= 800 && sitesChecked < 40; fi += kCavernCoarseLatticeRatio) {
            if (!cavernSiteGateOpen(kSeed, fi, fj)) continue;
            const CaveNode node = caveNode(kSeed, fi, fj);
            if (!cavernDepthIsSafe(node.depthMm)) continue;
            const CavernSite site = cavernSiteFor(kSeed, fi, fj, node, flatSurfaceAt);
            if (!site.valid) continue;
            ++sitesChecked;

            const int64_t anchorVx = floorDiv(node.xMm, int64_t(kVoxelSizeMm));
            const int64_t anchorVy = floorDiv(node.yMm, int64_t(kVoxelSizeMm));
            const int64_t anchorVz = floorDiv(site.anchorZMm - kVoxelSizeMm / 2, int64_t(kVoxelSizeMm));

            const CavernColumn cc = flatCavernColumnFor(anchorVx, anchorVy);
            const CaveColumn tc = caveColumnFor(kSeed, anchorVx, anchorVy, kFlatSurfaceMm, flatSurfaceAt);
            CHECK(cc.count > 0);
            CHECK(tc.count > 0); // backbone-crossing node -> 4 incident tunnels
            CHECK(cavernCarveAt(cc, kFlatSurfaceMm, kFlatBedrockMm, anchorVz));
            CHECK(caveCarveAt(tc, kFlatSurfaceMm, kFlatBedrockMm, anchorVz));
        }
    }
    CHECK(sitesChecked > 0);
    std::printf("    [caverns] anchor connectivity: %lld valid sites, every anchor voxel "
                "carved by BOTH systems\n",
                static_cast<long long>(sitesChecked));
}

// --- connectivity: flood fill, cavern component joins the tunnel network ----

VXC_TEST(cavern_flood_fill_shares_a_component_with_the_wider_tunnel_network) {
    const FoundSite fs = findFirstValidFlatSite();
    CHECK(fs.found);
    if (!fs.found) return;

    const int64_t anchorVx = floorDiv(fs.node.xMm, int64_t(kVoxelSizeMm));
    const int64_t anchorVy = floorDiv(fs.node.yMm, int64_t(kVoxelSizeMm));
    const int64_t anchorVz = floorDiv(fs.site.anchorZMm - kVoxelSizeMm / 2, int64_t(kVoxelSizeMm));

    constexpr int64_t kStep = 4;   // 0.4 m sample lattice, matches test_caves.cpp
    constexpr int64_t kNXY = 200;  // 200*4 = 800 voxels = 80 m (> 2x max reach)
    constexpr int64_t kNZ = 260;   // 260*4 = 1040 voxels = 104 m, generous vs the
                                    // widest observed chain span (~80 m)
    const int64_t x0 = anchorVx - kNXY * kStep / 2;
    const int64_t y0 = anchorVy - kNXY * kStep / 2;
    const int64_t z0 = anchorVz - kNZ * kStep / 2;

    const ConnectivityResult r = findComponents(
        [&](int64_t a, int64_t b, int64_t c) {
            return flatEitherCarve(x0 + a * kStep, y0 + b * kStep, z0 + c * kStep);
        },
        VoxelCoord{0, 0, 0}, VoxelCoord{kNXY - 1, kNXY - 1, kNZ - 1});

    const ComponentStats s = summarize(r);
    CHECK(s.total > 0);

    // Which component owns the anchor sample, and does that component reach
    // beyond the cavern's own max reach in xy (i.e. genuinely into the wider
    // tunnel network, not just be a big self-contained cavern bubble)?
    const int64_t sa = (anchorVx - x0) / kStep, sb = (anchorVy - y0) / kStep, sc = (anchorVz - z0) / kStep;
    int32_t anchorComp = -1;
    for (size_t ci = 0; ci < r.components.size(); ++ci) {
        for (const VoxelCoord& v : r.components[ci].voxels) {
            if (v.x == sa && v.y == sb && v.z == sc) {
                anchorComp = static_cast<int32_t>(ci);
                break;
            }
        }
        if (anchorComp >= 0) break;
    }
    CHECK(anchorComp >= 0);

    bool reachesBeyondCavern = false;
    if (anchorComp >= 0) {
        for (const VoxelCoord& v : r.components[size_t(anchorComp)].voxels) {
            const int64_t dvx = (v.x - sa) * kStep * kVoxelSizeMm;
            const int64_t dvy = (v.y - sb) * kStep * kVoxelSizeMm;
            if (dvx * dvx + dvy * dvy > kCavernMaxReachSqMm) {
                reachesBeyondCavern = true;
                break;
            }
        }
    }
    std::printf("    [caverns] flood fill at site (%lld,%lld): %d component(s), %zu "
                "samples total, anchor's component has %zu samples and %s beyond the "
                "cavern's own max reach\n",
                static_cast<long long>(fs.fi), static_cast<long long>(fs.fj), s.count,
                s.total, anchorComp >= 0 ? r.components[size_t(anchorComp)].size() : size_t(0),
                reachesBeyondCavern ? "DOES extend" : "does NOT extend");
    CHECK(reachesBeyondCavern);
}

// --- safety rule 1: bedrock is never breached, whatever depth it is at -----

VXC_TEST(cavern_never_breaches_bedrock) {
    // Driven directly against the (runtime-parameterised) clamp with several
    // different bedrock depths, including both today's amplifier range and a
    // deep, post-bedrock-move value -- this file makes no assumption about
    // which one is real, so it must hold for all of them.
    for (const int32_t bedrockDepthMm : {45000, 60000, 200000, 260000}) {
        int64_t sitesChecked = 0;
        int64_t closestApproachMm = 1LL << 60;
        for (int64_t fj = 0; fj <= 400 && sitesChecked < 20; fj += kCavernCoarseLatticeRatio) {
            for (int64_t fi = 0; fi <= 400 && sitesChecked < 20; fi += kCavernCoarseLatticeRatio) {
                if (!cavernSiteGateOpen(kSeed, fi, fj)) continue;
                const CaveNode node = caveNode(kSeed, fi, fj);
                if (!cavernDepthIsSafe(node.depthMm)) continue;
                const CavernSite site = cavernSiteFor(kSeed, fi, fj, node, flatSurfaceAt);
                if (!site.valid) continue;
                ++sitesChecked;

                const int64_t vx = floorDiv(node.xMm, int64_t(kVoxelSizeMm));
                const int64_t vy = floorDiv(node.yMm, int64_t(kVoxelSizeMm));
                const CavernColumn cc = flatCavernColumnFor(vx, vy);
                const int64_t topVz = floorDiv(kFlatSurfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
                for (int64_t vz = topVz; vz > topVz - 3000; --vz) {
                    if (!cavernCarveAt(cc, kFlatSurfaceMm, bedrockDepthMm, vz)) continue;
                    const int64_t depthMm =
                        int64_t(kFlatSurfaceMm) - (vz * kVoxelSizeMm + kVoxelSizeMm / 2);
                    const int64_t gap = int64_t(bedrockDepthMm) - depthMm;
                    CHECK(gap >= kCaveBedrockMarginMm);
                    if (gap < closestApproachMm) closestApproachMm = gap;
                }
            }
        }
        CHECK(sitesChecked > 0);
        std::printf("    [caverns] bedrock=%d mm: %lld sites, closest approach to bedrock "
                    "top %lld mm (margin %lld mm)\n",
                    bedrockDepthMm, static_cast<long long>(sitesChecked),
                    static_cast<long long>(closestApproachMm),
                    static_cast<long long>(kCaveBedrockMarginMm));
    }
}

// --- safety rule 2: sea level / ocean-beach guard ---------------------------

VXC_TEST(cavern_never_carves_at_or_below_sea_level_or_in_coastal_columns) {
    size_t oceanColumns = 0;
    for (int32_t surfaceMm = -40000; surfaceMm < kCavernMinSurfaceMm; surfaceMm += 271) {
        for (int64_t x = 0; x < 4096; x += 137) {
            for (int64_t y = 0; y < 4096; y += 151) {
                const CavernColumn c = cavernColumnFor(kSeed, x, y, surfaceMm, flatSurfaceAt);
                ++oceanColumns;
                for (int64_t vz = -600; vz < 200; vz += 11)
                    CHECK(!cavernCarveAt(c, surfaceMm, kFlatBedrockMm, vz));
            }
        }
    }
    CHECK(oceanColumns > 0);

    // Above the coastal threshold, tubes/caverns may exist, but never at or
    // below sea level, and never when the SITE itself sits below
    // kCavernMinSurfaceMm even if the querying column's own surface is fine.
    size_t subSeaChecks = 0;
    for (int64_t x = 0; x < 8192; x += 137)
        for (int64_t y = 0; y < 8192; y += 151) {
            const CavernColumn c = flatCavernColumnFor(x, y);
            for (int64_t vz = -400; vz < kCaveMinVoxelZ; vz += 3) {
                CHECK(!cavernCarveAt(c, kFlatSurfaceMm, kFlatBedrockMm, vz));
                ++subSeaChecks;
            }
        }
    CHECK(subSeaChecks > 0);
    std::printf("    [caverns] ocean: %zu synthetic sub-threshold columns produced zero "
                "carves; %zu sub-sea-level probes all refused\n",
                oceanColumns, subSeaChecks);
}

// --- safety rule 3: roof cover on GENUINELY VARIED (sloped) real terrain ---

VXC_TEST(cavern_roof_clamp_is_load_bearing_on_sloped_real_terrain) {
    // Matt's explicit post-design ask: verify on real, varied terrain (not
    // flat ground) that the roof/bedrock/sea-level clamps actually do their
    // job when a level-anchored cavern sits under a slope, and report the
    // measured minimum roof cover. Uses the real Amplifier + its own
    // bilinear-tile-base + detail-octave surface function as the `surfaceAt`
    // callback (via the public column() API), so this exercises exactly the
    // kind of terrain caverns will actually generate under in production.
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);

    auto realSurfaceAt = [&](int64_t xMm, int64_t yMm) -> int32_t {
        const int64_t vx = floorDiv(xMm, int64_t(kVoxelSizeMm));
        const int64_t vy = floorDiv(yMm, int64_t(kVoxelSizeMm));
        return amp.column(vx, vy).surfaceMm;
    };

    // "Is a voxel inside a room's ELLIPSOID (plus its flat floor), ignoring
    // every clamp?" â€” the same per-seg test cavernCarveAt runs, minus the
    // guards. The difference between this and cavernCarveAt is exactly the
    // set of voxels a clamp truncated, which is what "load-bearing" means.
    auto geometricallyInsideARoom = [](const CavernColumn& c, int64_t vz) {
        const int64_t zAbs = vz * kVoxelSizeMm + kVoxelSizeMm / 2;
        for (int32_t s = 0; s < c.count; ++s) {
            const CavernSeg& sg = c.segs[s];
            if (zAbs < int64_t(sg.zFloorMm)) continue;
            const int64_t dz = zAbs - int64_t(sg.zCenterMm);
            if (dz * dz < int64_t(sg.marginSq)) return true;
        }
        return false;
    };

    int64_t columnsSampled = 0, columnsWithCavern = 0, carvedVoxels = 0;
    int64_t columnsFullyRefused = 0; // in reach, but every candidate voxel was clamped away
    int64_t clampedVoxels = 0;       // inside a room's geometry, but a clamp refused it
    int64_t columnsPartlyClamped = 0;
    int64_t thinnestRoofMm = 1LL << 60;
    int32_t minSurfaceMmSeen = INT32_MAX, maxSurfaceMmSeen = INT32_MIN;

    // A ~1.6 km x 1.6 km scan at 2 m resolution: coarse enough to run fast,
    // fine enough that a 24-56 m wide cavern is sampled many times over, and
    // wide enough to cross several 204.8 m coarse cells (multiple candidate
    // sites, some certainly astride sloped ground given SyntheticTileSampler
    // has real hills at up to 480 m wavelength).
    for (int64_t x = -8000; x < 8000; x += 20) {
        for (int64_t y = -8000; y < 8000; y += 20) {
            ++columnsSampled;
            const ColumnSample col = amp.column(x, y);
            if (col.surfaceMm < minSurfaceMmSeen) minSurfaceMmSeen = col.surfaceMm;
            if (col.surfaceMm > maxSurfaceMmSeen) maxSurfaceMmSeen = col.surfaceMm;

            const CavernColumn cc = cavernColumnFor(kSeed, x, y, col.surfaceMm, realSurfaceAt);
            if (cc.count == 0) continue;

            bool any = false, anyClamped = false;
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            for (int64_t vz = topVz; vz > topVz - 3000; --vz) {
                const bool carved = cavernCarveAt(cc, col.surfaceMm, col.bedrockDepthMm, vz);
                if (!carved) {
                    if (geometricallyInsideARoom(cc, vz)) {
                        ++clampedVoxels;
                        anyClamped = true;
                    }
                    continue;
                }
                any = true;
                ++carvedVoxels;
                const int64_t roofMm =
                    int64_t(col.surfaceMm) - (vz * kVoxelSizeMm + kVoxelSizeMm / 2);
                if (roofMm < thinnestRoofMm) thinnestRoofMm = roofMm;
            }
            if (anyClamped) ++columnsPartlyClamped;
            if (any)
                ++columnsWithCavern;
            else
                ++columnsFullyRefused;
        }
    }

    std::printf("    [caverns] sloped real terrain: surface ranged %d..%d mm (%.1f m of "
                "relief) over the scan\n",
                minSurfaceMmSeen, maxSurfaceMmSeen,
                (maxSurfaceMmSeen - minSurfaceMmSeen) / 1000.0);
    std::printf("    [caverns] sloped real terrain: %lld columns sampled, %lld carved "
                "%lld voxels, %lld were in a cavern's reach but fully refused by the "
                "clamps (slope truncation)\n",
                static_cast<long long>(columnsSampled), static_cast<long long>(columnsWithCavern),
                static_cast<long long>(carvedVoxels), static_cast<long long>(columnsFullyRefused));
    std::printf("    [caverns] sloped real terrain: %lld voxels sat inside a room's "
                "ellipsoid but were refused by a clamp, across %lld columns (%.1f%% of "
                "columns over a cavern)\n",
                static_cast<long long>(clampedVoxels), static_cast<long long>(columnsPartlyClamped),
                columnsWithCavern > 0
                    ? 100.0 * double(columnsPartlyClamped) / double(columnsWithCavern)
                    : 0.0);

    CHECK(maxSurfaceMmSeen > minSurfaceMmSeen); // genuinely varied, not flat
    CHECK(carvedVoxels > 0);                    // caverns do exist in this scan
    CHECK(thinnestRoofMm < (1LL << 60));
    if (thinnestRoofMm < (1LL << 60)) {
        std::printf("    [caverns] sloped real terrain: MEASURED minimum roof cover over "
                    "any carved cavern voxel = %lld mm (%.2f m); clamp = %lld mm\n",
                    static_cast<long long>(thinnestRoofMm), static_cast<double>(thinnestRoofMm) / 1000.0,
                    static_cast<long long>(kCaveRoofMinMm));
        CHECK(thinnestRoofMm >= kCaveRoofMinMm); // the clamp held, by construction
    }
    // The whole point of this test: on sloped ground, the clamps actually BIND
    // somewhere -- the "load-bearing, not just backstop" claim, measured.
    //
    // This assertion USED to be `columnsFullyRefused > 0` (an in-reach column
    // where every candidate voxel was clamped away). That proxy stopped firing
    // at kWorldGenVersion 5, and for a good reason rather than a regression:
    // with bedrock moved from 40-60 m to 180-220 m, the BEDROCK clamp no
    // longer truncates any cavern at all (the deepest a chain reaches is
    // ~128 m), so the only clamp still binding is the roof -- and the roof
    // clamp shaves the TOP off a room rather than erasing a whole column's
    // worth of it. Counting fully-erased columns therefore measures a clamp
    // that is now correctly inert; counting CLAMPED VOXELS measures the one
    // that is still load-bearing, which is what the claim was always about.
    CHECK_EQ(columnsFullyRefused, 0); // pinned: no clamp erases a whole column any more
    CHECK(clampedVoxels > 0);         // but the roof clamp does still truncate real rooms
    CHECK(columnsPartlyClamped > 0);
}

// --- storage bound ------------------------------------------------------------

VXC_TEST(cavern_segment_cap_headroom) {
    int32_t maxSegs = 0;
    size_t columns = 0, withAny = 0;
    for (int64_t x = -80000; x < 80000; x += 37)
        for (int64_t y = -80000; y < 80000; y += 43) {
            const CavernColumn c = flatCavernColumnFor(x, y);
            ++columns;
            if (c.count > 0) ++withAny;
            if (c.count > maxSegs) maxSegs = c.count;
        }
    CHECK(maxSegs > 0);
    // kMaxCavernSegs was shrunk to exactly kCavernChildCount (the provable
    // max -- see caverns.h's static_assert "at most one open, in-reach site
    // can ever cover a given column" x at most kCavernChildCount rooms per
    // site): there is no headroom left by design, so the meaningful checks
    // are "never exceeds the theoretical bound" and "the storage cap really
    // is that bound, not something smaller that would silently truncate".
    CHECK(maxSegs <= kCavernChildCount);
    CHECK_EQ(kMaxCavernSegs, kCavernChildCount);
    std::printf("    [caverns] segment cap: max %d rooms per column over %zu columns "
                "(cap %d); %.3f%% of columns are over a cavern\n",
                maxSegs, columns, kMaxCavernSegs, 100.0 * double(withAny) / double(columns));
}

// --- flood level properties --------------------------------------------------

VXC_TEST(cavern_flood_level_is_level_disc_consistent_bounded_and_roughly_forty_percent_dry) {
    int64_t sitesChecked = 0, wetSites = 0;
    for (int64_t fj = 0; fj <= 1600 && sitesChecked < 150; fj += kCavernCoarseLatticeRatio) {
        for (int64_t fi = 0; fi <= 1600 && sitesChecked < 150; fi += kCavernCoarseLatticeRatio) {
            if (!cavernSiteGateOpen(kSeed, fi, fj)) continue;
            const CaveNode node = caveNode(kSeed, fi, fj);
            if (!cavernDepthIsSafe(node.depthMm)) continue;
            const CavernSite site = cavernSiteFor(kSeed, fi, fj, node, flatSurfaceAt);
            if (!site.valid) continue;
            ++sitesChecked;
            if (site.floodZMm == INT32_MIN) continue;
            ++wetSites;

            CHECK(site.floodZMm > 0);
            CHECK(int64_t(site.floodZMm) < site.anchorZMm);

            // Disc-consistency: several columns within the reach disc (but
            // not necessarily overlapping any room) must report the SAME
            // flood level as the site itself -- "whether or not a cavern
            // seg overlaps it" per design doc Â§5.1.
            const int64_t vx0 = floorDiv(node.xMm, int64_t(kVoxelSizeMm));
            const int64_t vy0 = floorDiv(node.yMm, int64_t(kVoxelSizeMm));
            bool sawIt = false;
            for (int64_t dvx : {-50, 0, 50}) {
                for (int64_t dvy : {-50, 0, 50}) {
                    const CavernColumn cc = flatCavernColumnFor(vx0 + dvx, vy0 + dvy);
                    if (cc.floodZMm == INT32_MIN) continue;
                    CHECK_EQ(cc.floodZMm, site.floodZMm);
                    sawIt = true;
                }
            }
            CHECK(sawIt);
        }
    }
    CHECK(sitesChecked > 0);
    const double wetFrac = double(wetSites) / double(sitesChecked);
    std::printf("    [caverns] flood: %lld/%lld sites wet (%.1f%%, target ~60%%)\n",
                static_cast<long long>(wetSites), static_cast<long long>(sitesChecked),
                100.0 * wetFrac);
    CHECK(wetFrac > 0.45);
    CHECK(wetFrac < 0.75);
}

// --- cost budget --------------------------------------------------------------

VXC_TEST(cavern_cost_budget_micro_benchmark) {
    // Standalone timing, same spirit as docs/cavern-design.md Â§3.7's
    // prototype: clang -O2, best of 3, a volatile sink to defeat dead-code
    // elimination. Not a hard CI gate (no timing test in this suite is), but
    // reported so the actual numbers are on record.
    constexpr int64_t kN = 400000;
    volatile int64_t sink = 0;

    // Per-column cost: a realistic mix of columns (most far from any site).
    double bestColumnNs = 1e18;
    for (int rep = 0; rep < 3; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        int64_t acc = 0;
        for (int64_t k = 0; k < kN; ++k) {
            const int64_t x = k * 137 - 20000000;
            const int64_t y = k * 419 - 20000000;
            const CavernColumn c = flatCavernColumnFor(x, y);
            acc += c.count + c.floodZMm;
        }
        const auto t1 = std::chrono::steady_clock::now();
        sink = acc;
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / double(kN);
        if (ns < bestColumnNs) bestColumnNs = ns;
    }

    // Per-voxel cost, cavern-free column (the overwhelmingly common case):
    // pick a column definitely not near any site.
    const CavernColumn emptyCol = flatCavernColumnFor(3, 5);
    CHECK_EQ(emptyCol.count, 0);
    double bestEmptyVoxelNs = 1e18;
    constexpr int64_t kNVoxel = 2000000;
    for (int rep = 0; rep < 3; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        int64_t acc = 0;
        for (int64_t k = 0; k < kNVoxel; ++k)
            acc += cavernCarveAt(emptyCol, kFlatSurfaceMm, kFlatBedrockMm, 560 + (k % 400)) ? 1 : 0;
        const auto t1 = std::chrono::steady_clock::now();
        sink = acc;
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / double(kNVoxel);
        if (ns < bestEmptyVoxelNs) bestEmptyVoxelNs = ns;
    }

    // Per-voxel cost, a column actually under a cavern (worst case in this
    // sample -- whatever segment count the found site happens to produce).
    const FoundSite fs = findFirstValidFlatSite();
    CHECK(fs.found);
    double bestFullVoxelNs = 1e18;
    int32_t fullColSegs = 0;
    if (fs.found) {
        const int64_t vx = floorDiv(fs.node.xMm, int64_t(kVoxelSizeMm));
        const int64_t vy = floorDiv(fs.node.yMm, int64_t(kVoxelSizeMm));
        const CavernColumn fullCol = flatCavernColumnFor(vx, vy);
        fullColSegs = fullCol.count;
        CHECK(fullColSegs > 0);
        for (int rep = 0; rep < 3; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            int64_t acc = 0;
            for (int64_t k = 0; k < kNVoxel; ++k)
                acc += cavernCarveAt(fullCol, kFlatSurfaceMm, kFlatBedrockMm, 400 + (k % 800)) ? 1 : 0;
            const auto t1 = std::chrono::steady_clock::now();
            sink = acc;
            const double ns =
                std::chrono::duration<double, std::nano>(t1 - t0).count() / double(kNVoxel);
            if (ns < bestFullVoxelNs) bestFullVoxelNs = ns;
        }
    }

    (void)sink;
    std::printf("    [caverns] cost: cavernColumnFor %.2f ns/column (mixed columns); "
                "cavernCarveAt %.3f ns/voxel (count==0 fast path); %.3f ns/voxel (%d "
                "segs, worst case in this sample)\n",
                bestColumnNs, bestEmptyVoxelNs, bestFullVoxelNs, fullColSegs);

    // Budget per docs/cavern-design.md Â§3.7: the common-case per-voxel cost
    // should be a hair above a single compare (a few ns is plenty of
    // headroom); worst-case per-voxel should stay well under ~4 ns/voxel;
    // per-column cost should be modest (the site is 4x rarer than the
    // original spec, so the full-reduction tier fires even less often than
    // the design's own measured ~4%).
    CHECK(bestEmptyVoxelNs < 5.0);
    CHECK(bestFullVoxelNs < 8.0);
    CHECK(bestColumnNs < 500.0);
}
