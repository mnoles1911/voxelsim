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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/cave_families.h"
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
    // kWorldGenVersion 26 (plan W4) is the first version to move it: the
    // chamber shape changed (offsets, elongation, pillars, breakdown), so
    // every field this digest walks -- marginSq, zCenterMm, zFloorMm and the
    // segment count -- moves with it. Pinned against a CONSTANT surface, so
    // this number is a statement about the cavern construct alone; nothing
    // about terrain can drag it.
    // (was 0xFB45CBD3F95E65C4 at v1..v25)
    CHECK_EQ(d.h, 0xB87B2128B39244CDull);
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
    int64_t minChainOverlapMm = 1ll << 40;
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
                    const int64_t dz = prev.zMm - ch.zMm;
                    CHECK(dz > 0); // strictly descending

                    // v26 WITNESS-COLUMN OVERLAP, measured -- the direct
                    // counterpart of caverns.h's static_asserts (a), (b) and
                    // (b'), evaluated at real hashed sites instead of at
                    // worst-case constants.
                    //
                    // The v25 form of this check was the coaxial 1D test
                    // `dz < prev.rz + ch.rz`. That is no longer the right
                    // question -- the rooms are not coaxial -- and, worse, it
                    // still PASSES on v26 geometry, because a small offset
                    // barely moves the projected intervals. A check that
                    // survives the change it is supposed to police is a check
                    // that has stopped testing anything, so it is replaced
                    // rather than kept alongside.
                    //
                    // At the child's own axis: the child contributes its full
                    // rz; the parent contributes h, its vertical half-extent
                    // at that xy, taken at the WORST possible roughness draw.
                    const int64_t rx = ch.xMm - prev.xMm;
                    const int64_t ry = ch.yMm - prev.yMm;
                    const int64_t alongMm =
                        cavernAbs(rx * prev.dirCosQ12 + ry * prev.dirSinQ12) >> kCavernDirShift;
                    const int64_t acrossMm =
                        ((cavernAbs(ry * prev.dirCosQ12 - rx * prev.dirSinQ12) >>
                          kCavernDirShift) *
                         prev.elongQ10) /
                        1024;
                    const int64_t dEffSq = alongMm * alongMm + acrossMm * acrossMm;
                    const int64_t rSqRough =
                        prev.rxyMm * prev.rxyMm * kCavernRoughMinQ10 / 1024;
                    CHECK(dEffSq < rSqRough); // (a): the child's axis is inside the parent
                    const int64_t hMm =
                        cavernIsqrt(prev.rzMm * prev.rzMm * (rSqRough - dEffSq) / rSqRough);
                    // The overlap interval at that column is
                    // [max(ch.z - ch.rz, prev.z - h), min(ch.z + ch.rz, prev.z + h)].
                    const int64_t lo = std::max(ch.zMm - ch.rzMm, prev.zMm - hMm);
                    const int64_t hi = std::min(ch.zMm + ch.rzMm, prev.zMm + hMm);
                    CHECK(hi - lo >= kCavernMinChainOverlapMm);
                    // ...and it has to survive the rubble, which raises both
                    // rooms' floors under that same column.
                    const int64_t floorCap = std::max(ch.zFloorMm, prev.zFloorMm) +
                                             kCavernBreakdownMaxMm;
                    CHECK(hi > floorCap);
                    if (hi - lo < minChainOverlapMm) minChainOverlapMm = hi - lo;
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
                "overlaps hold; widest observed vertical span %.1f m; tightest measured "
                "room-to-room overlap at the child axis %.2f m (floor %.2f m)\n",
                static_cast<long long>(sitesChecked), static_cast<double>(maxSpanMm) / 1000.0,
                static_cast<double>(minChainOverlapMm) / 1000.0,
                static_cast<double>(kCavernMinChainOverlapMm) / 1000.0);
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

    // THE CRITERION IS "REACHES REAL TUNNEL", NOT "REACHES A DISTANCE.
    //
    // It used to be the latter: a sample further from the anchor than
    // kCavernMaxReachMm. That is a WORST-CASE constant over every possible
    // hash draw, not this site's actual size, and it silently became
    // impossible to satisfy at worldgen v26 -- the leaning chain raised the
    // worst-case reach to 57 m while this box is only 80 m across, so no
    // sample inside it can be further from the anchor than the bound. The
    // test failed while the flood fill was reporting 729,037 of 735,258
    // samples in the anchor's single component, i.e. it failed on its own
    // yardstick rather than on connectivity.
    //
    // What the test is actually for is "the cavern is not a self-contained
    // bubble", so ask that directly: some sample in the anchor's component
    // must be carved by the TUNNEL system and by no cavern room at all. That
    // is a statement about the geometry under test rather than about a
    // constant, and it cannot be satisfied by making the rooms bigger.
    bool reachesTunnel = false;
    int64_t furthestMm = 0;
    if (anchorComp >= 0) {
        for (const VoxelCoord& v : r.components[size_t(anchorComp)].voxels) {
            const int64_t wx = x0 + v.x * kStep, wy = y0 + v.y * kStep, wz = z0 + v.z * kStep;
            const int64_t dvx = (v.x - sa) * kStep * kVoxelSizeMm;
            const int64_t dvy = (v.y - sb) * kStep * kVoxelSizeMm;
            const int64_t dSq = dvx * dvx + dvy * dvy;
            if (dSq > furthestMm * furthestMm) furthestMm = cavernIsqrt(dSq);
            if (reachesTunnel) continue;
            if (flatCavernCarve(wx, wy, wz)) continue; // inside a room: not the proof
            const CaveColumn tc = caveColumnFor(kSeed, wx, wy, kFlatSurfaceMm, flatSurfaceAt);
            if (caveCarveAt(tc, kFlatSurfaceMm, kFlatBedrockMm, wz)) reachesTunnel = true;
        }
    }
    std::printf("    [caverns] flood fill at site (%lld,%lld): %d component(s), %zu "
                "samples total, anchor's component has %zu samples reaching %.1f m from the "
                "anchor and %s pure-tunnel voxels outside every cavern room\n",
                static_cast<long long>(fs.fi), static_cast<long long>(fs.fj), s.count,
                s.total, anchorComp >= 0 ? r.components[size_t(anchorComp)].size() : size_t(0),
                furthestMm / 1000.0, reachesTunnel ? "DOES include" : "does NOT include");
    CHECK(reachesTunnel);
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

// --- W4 (v26): plan-view symmetry, against a control ------------------------
//
// THE ACCEPTANCE GATE FOR W4, AND WHY IT IS A DIFFERENCE AND NOT A NUMBER.
//
// The plan's Verify line for this wave is "plan-view symmetry visibly broken."
// A statistic that only says "the footprint is asymmetric" cannot carry that,
// because a v25 cavern footprint ALREADY reads asymmetric whenever the roof
// clamp truncates it against a slope, the bedrock clamp cuts its bottom off,
// or the sampled window clips it. Reporting one arm would repeat exactly the
// error W3 made with its "thin roof means horizontal mouth" figure: a number
// satisfied by geometry other than the one under test.
//
// So every statistic below is measured TWICE on the same sites, the same
// seed, the same terrain and the same shipping predicate, with the only
// difference being cave_families.h's `cavernSiteWithoutChamberShape` — v25's
// coaxial, round, pillar-free, flat-floored chamber out of the v26 world.
// The control arm is what proves each statistic can report "symmetric" at
// all; the real arm is what shows it does not. If W4's terms were ever
// deleted the two arms would coincide and this test fails, which is the
// property a gate has to have.
//
// Flat terrain deliberately: it removes the three contaminating clips
// outright, so what is left is the chamber and nothing else.
namespace {

struct PlanStats {
    int64_t areaCols = 0;
    int64_t anisoQ10 = 1024;   // lambda_max / lambda_min of the plan mask, Q10
    int64_t driftMm = 0;       // furthest apart two room-centre slice centroids sit
    int64_t holes = 0;         // enclosed rock islands in plan == pillars
    int64_t d180PerMille = 0;  // cells with no partner under a half turn about the centroid
    int64_t floorStepMm = 0;   // mean |floor difference| between 4-adjacent mask columns
};

// Plan-view statistics for one site, from the SHIPPING predicate.
//
// A column is "in the mask" if any room's own centre voxel carves there --
// exact, because marginSq > 0 already means dz == 0 satisfies the ellipsoid
// test and a room centre is always above its own floor, so this asks
// cavernCarveAt rather than re-deriving anything. It is also what makes
// pillars visible here: a pillar column emits no segments at all.
template <typename ColFn>
PlanStats planStatsFor(const CavernSite& site, const ColFn& colAt, int64_t stepMm) {
    const int32_t kN = 121; // 121 x 1 m = 121 m, wider than 2x the max reach
    const int64_t half = kN / 2;
    std::vector<uint8_t> mask(size_t(kN) * kN, 0);
    // Floor height PER ROOM, not the column's lowest floor. The first version
    // of this took the minimum over the column's segments, and the control arm
    // then read HIGHER than the real one (1200 mm vs 534 mm) -- because that
    // number is dominated by the step where one room's footprint ends and the
    // next room's deeper floor takes over, which is chain geometry and not
    // rubble at all. Asked per room the v25 floor is a plane and the statistic
    // is exactly 0, which is what makes the real arm's value mean "breakdown".
    std::vector<int64_t> floorZ(size_t(kN) * kN * kCavernChildCount, 0);
    std::vector<uint8_t> hasRoom(size_t(kN) * kN * kCavernChildCount, 0);
    // One plan slice per room centre. Two rooms whose slices have the same
    // centroid are stacked on one axis; that is the v25 chamber, and the
    // spread of these centroids is the statistic that says so.
    int64_t sliceSumX[kCavernChildCount] = {}, sliceSumY[kCavernChildCount] = {},
            sliceN[kCavernChildCount] = {};

    PlanStats st;
    int64_t sumX = 0, sumY = 0;
    for (int32_t iy = 0; iy < kN; ++iy)
        for (int32_t ix = 0; ix < kN; ++ix) {
            const int64_t xMm = site.anchorXMm + (ix - half) * stepMm;
            const int64_t yMm = site.anchorYMm + (iy - half) * stepMm;
            const int64_t vx = floorDiv(xMm, int64_t(kVoxelSizeMm));
            const int64_t vy = floorDiv(yMm, int64_t(kVoxelSizeMm));
            const CavernColumn c = colAt(vx, vy);
            if (c.count == 0) continue;
            bool any = false;
            for (int32_t s = 0; s < c.count; ++s) {
                const int64_t vz = floorDiv(int64_t(c.segs[s].zCenterMm) - kVoxelSizeMm / 2,
                                            int64_t(kVoxelSizeMm));
                if (!cavernCarveAt(c, kFlatSurfaceMm, kFlatBedrockMm, vz)) continue;
                any = true;
                for (int32_t k = 0; k < kCavernChildCount; ++k)
                    if (c.segs[s].zCenterMm == int32_t(site.children[k].zMm)) {
                        sliceSumX[k] += xMm;
                        sliceSumY[k] += yMm;
                        ++sliceN[k];
                        const size_t fi2 = (size_t(iy) * kN + ix) * kCavernChildCount + k;
                        floorZ[fi2] = c.segs[s].zFloorMm;
                        hasRoom[fi2] = 1;
                    }
            }
            if (!any) continue;
            mask[size_t(iy) * kN + ix] = 1;
            sumX += ix;
            sumY += iy;
            ++st.areaCols;
        }
    if (st.areaCols == 0) return st;

    // (1) ANISOTROPY -- "round in plan", the first tell. Second moments of the
    // mask; the eigenvalue ratio of a 2x2 symmetric matrix in closed form.
    const int64_t cx2 = 2 * sumX / st.areaCols, cy2 = 2 * sumY / st.areaCols; // doubled centroid
    int64_t mxx = 0, myy = 0, mxy = 0;
    for (int32_t iy = 0; iy < kN; ++iy)
        for (int32_t ix = 0; ix < kN; ++ix) {
            if (!mask[size_t(iy) * kN + ix]) continue;
            const int64_t dx = 2 * ix - cx2, dy = 2 * iy - cy2; // doubled, so integer
            mxx += dx * dx;
            myy += dy * dy;
            mxy += dx * dy;
        }
    mxx /= st.areaCols;
    myy /= st.areaCols;
    mxy /= st.areaCols;
    {
        const int64_t tr = mxx + myy;
        const int64_t disc = cavernIsqrt((mxx - myy) * (mxx - myy) + 4 * mxy * mxy);
        const int64_t lo = tr - disc;
        st.anisoQ10 = lo > 0 ? (tr + disc) * 1024 / lo : (1024 * 64);
    }

    // (2) STACK DRIFT -- "the rooms share one axis", the second tell. Largest
    // plan distance between any two room-centre slice centroids. Exactly zero
    // for a coaxial chain, by construction and not by luck.
    for (int32_t a = 0; a < kCavernChildCount; ++a)
        for (int32_t b = a + 1; b < kCavernChildCount; ++b) {
            if (sliceN[a] == 0 || sliceN[b] == 0) continue;
            const int64_t dx = sliceSumX[a] / sliceN[a] - sliceSumX[b] / sliceN[b];
            const int64_t dy = sliceSumY[a] / sliceN[a] - sliceSumY[b] / sliceN[b];
            const int64_t d = cavernIsqrt(dx * dx + dy * dy);
            if (d > st.driftMm) st.driftMm = d;
        }

    // (3) HOLES -- pillars, counted topologically rather than by asking the
    // pillar field whether it fired. A hole is a 4-connected run of non-mask
    // cells that never reaches the grid border, i.e. rock completely enclosed
    // by chamber in plan. Nothing in a union of ellipses can produce one.
    {
        std::vector<uint8_t> seen(size_t(kN) * kN, 0);
        std::vector<int32_t> stack;
        for (int32_t iy = 0; iy < kN; ++iy)
            for (int32_t ix = 0; ix < kN; ++ix) {
                const int32_t s0 = iy * kN + ix;
                if (mask[size_t(s0)] || seen[size_t(s0)]) continue;
                bool touchesBorder = false;
                stack.clear();
                stack.push_back(s0);
                seen[size_t(s0)] = 1;
                while (!stack.empty()) {
                    const int32_t s = stack.back();
                    stack.pop_back();
                    const int32_t sx = s % kN, sy = s / kN;
                    if (sx == 0 || sy == 0 || sx == kN - 1 || sy == kN - 1) touchesBorder = true;
                    const int32_t nb[4] = {sx > 0 ? s - 1 : -1, sx < kN - 1 ? s + 1 : -1,
                                           sy > 0 ? s - kN : -1, sy < kN - 1 ? s + kN : -1};
                    for (int32_t k = 0; k < 4; ++k) {
                        if (nb[k] < 0 || mask[size_t(nb[k])] || seen[size_t(nb[k])]) continue;
                        seen[size_t(nb[k])] = 1;
                        stack.push_back(nb[k]);
                    }
                }
                if (!touchesBorder) ++st.holes;
            }
    }

    // (4) HALF-TURN DEFECT -- general plan point symmetry. The doubled
    // centroid keeps the reflection an exact grid map, so no interpolation
    // enters and the number is not an artefact of resampling.
    {
        int64_t unmatched = 0;
        for (int32_t iy = 0; iy < kN; ++iy)
            for (int32_t ix = 0; ix < kN; ++ix) {
                if (!mask[size_t(iy) * kN + ix]) continue;
                // Point reflection through the centroid c is p = 2c - i. The
                // centroid is carried DOUBLED (cx2 == 2c) for exactly this
                // reason: 2c - i is then an integer grid cell, so the map is
                // exact and no resampling can manufacture a defect.
                const int64_t px = cx2 - ix, py = cy2 - iy;
                if (px < 0 || py < 0 || px >= kN || py >= kN) {
                    ++unmatched;
                    continue;
                }
                if (!mask[size_t(py) * kN + size_t(px)]) ++unmatched;
            }
        st.d180PerMille = unmatched * 1000 / st.areaCols;
    }

    // (5) FLOOR STEP -- breakdown. Mean absolute floor difference between
    // 4-adjacent columns THAT CONTAIN THE SAME ROOM. A machined flat floor
    // gives exactly 0; rubble gives a value everywhere. See the floorZ
    // declaration for why "the same room" is the load-bearing part.
    {
        int64_t sum = 0, pairs = 0;
        for (int32_t k = 0; k < kCavernChildCount; ++k)
            for (int32_t iy = 0; iy < kN; ++iy)
                for (int32_t ix = 0; ix + 1 < kN; ++ix) {
                    const size_t a = (size_t(iy) * kN + ix) * kCavernChildCount + k;
                    const size_t b = a + kCavernChildCount;
                    if (!hasRoom[a] || !hasRoom[b]) continue;
                    sum += floorZ[a] > floorZ[b] ? floorZ[a] - floorZ[b] : floorZ[b] - floorZ[a];
                    ++pairs;
                }
        st.floorStepMm = pairs ? sum / pairs : 0;
    }
    return st;
}

} // namespace

VXC_TEST(cavern_plan_symmetry_is_broken_and_the_control_proves_the_statistic_can_see_symmetry) {
    int64_t sites = 0;
    int64_t realAnisoSum = 0, plainAnisoSum = 0;
    int64_t realDriftSum = 0, plainDriftMax = 0;
    int64_t realHolesTotal = 0, plainHolesTotal = 0, sitesWithPillars = 0;
    int64_t realD180Sum = 0, plainD180Sum = 0;
    int64_t realFloorSum = 0, plainFloorMax = 0;
    int64_t anisoBothWays = 0;

    for (int64_t fj = 0; fj <= 800 && sites < 12; fj += kCavernCoarseLatticeRatio) {
        for (int64_t fi = 0; fi <= 800 && sites < 12; fi += kCavernCoarseLatticeRatio) {
            if (!cavernSiteGateOpen(kSeed, fi, fj)) continue;
            const CaveNode node = caveNode(kSeed, fi, fj);
            if (!cavernDepthIsSafe(node.depthMm)) continue;
            const CavernSite site = cavernSiteFor(kSeed, fi, fj, node, flatSurfaceAt);
            if (!site.valid) continue;
            ++sites;

            const int64_t si = floorDiv(site.anchorXMm, kCavernCoarseMm);
            const int64_t sj = floorDiv(site.anchorYMm, kCavernCoarseMm);
            // The candidate block the anchor's own cell sees. Both arms go
            // through the SAME shipping reduction with the SAME candidates;
            // only the site's shape fields differ.
            const CavernCandidates cands = cavernCandidatesFor(kSeed, si, sj);
            const PlanStats real = planStatsFor(site, [&](int64_t vx, int64_t vy) {
                return cavernColumnFromCandidates(kSeed, cands, vx, vy, flatSurfaceAt);
            }, 1000);
            const PlanStats plain = planStatsFor(site, [&](int64_t vx, int64_t vy) {
                return cavernColumnWithoutChamberShape(kSeed, cands, vx, vy, flatSurfaceAt);
            }, 1000);
            if (real.areaCols == 0 || plain.areaCols == 0) continue;

            realAnisoSum += real.anisoQ10;
            plainAnisoSum += plain.anisoQ10;
            realDriftSum += real.driftMm;
            if (plain.driftMm > plainDriftMax) plainDriftMax = plain.driftMm;
            realHolesTotal += real.holes;
            plainHolesTotal += plain.holes;
            if (real.holes > 0) ++sitesWithPillars;
            realD180Sum += real.d180PerMille;
            plainD180Sum += plain.d180PerMille;
            realFloorSum += real.floorStepMm;
            if (plain.floorStepMm > plainFloorMax) plainFloorMax = plain.floorStepMm;
            if (real.anisoQ10 > plain.anisoQ10) ++anisoBothWays;
        }
    }
    CHECK(sites >= 8);
    if (sites == 0) return;

    const double aReal = double(realAnisoSum) / double(sites) / 1024.0;
    const double aPlain = double(plainAnisoSum) / double(sites) / 1024.0;
    std::printf("    [caverns] W4 plan symmetry over %lld flat sites, v26 vs the v25-shape "
                "control:\n"
                "               anisotropy   %.2f : 1   vs   %.2f : 1   (control ~1 = round)\n"
                "               stack drift  %.1f m      vs   %.1f m     (control 0 = coaxial)\n"
                "               plan holes   %lld total  vs   %lld       (holes ARE pillars)\n"
                "               half-turn defect %lld/1000 vs %lld/1000\n"
                "               floor step   %lld mm     vs   %lld mm    (control 0 = machined)\n",
                static_cast<long long>(sites), aReal, aPlain,
                double(realDriftSum) / double(sites) / 1000.0, double(plainDriftMax) / 1000.0,
                static_cast<long long>(realHolesTotal), static_cast<long long>(plainHolesTotal),
                static_cast<long long>(realD180Sum / sites),
                static_cast<long long>(plainD180Sum / sites),
                static_cast<long long>(realFloorSum / sites),
                static_cast<long long>(plainFloorMax));

    // THE CONTROL SIDE FIRST. These are the assertions that make the real
    // side mean something; if any of them fails, the statistic is reading
    // something other than the chamber and the real side's numbers are not
    // evidence of anything.
    //
    // Two of the four control bounds are NOT zero, and the difference between
    // them is the honest part of this gate. The floor step is exactly zero
    // because a v25 floor really is a plane. The others are not, because the
    // wall-roughness noise -- which v25 already had -- wobbles a disc's
    // boundary by up to 15% of its radius: that moves a slice centroid by
    // over a metre and can occasionally pinch off a one-cell rock island. So
    // those two bounds are stated as the statistic's measured NOISE FLOOR,
    // and the real arm has to beat the floor by a wide margin rather than
    // merely be above zero. Pretending the floor was zero would have made
    // this gate assert something false about the control and then read the
    // real arm's margin as larger than it is.
    CHECK(plainAnisoSum / sites < 1024 * 115 / 100); // a concentric disc is round in plan
    CHECK(plainDriftMax < 2500);                     // roughness wobble only; no real axis drift
    CHECK(plainHolesTotal * 4 < 12);                 // ellipsoids enclose essentially no rock
    CHECK_EQ(plainFloorMax, 0);                      // the v25 floor is a machined plane, exactly

    // ...and the real side, each beating the control by a margin rather than
    // beating zero.
    CHECK(realAnisoSum / sites >= 1024 * 135 / 100); // rooms are ellipses, not circles
    CHECK(realAnisoSum >= plainAnisoSum * 3 / 2);
    CHECK(realDriftSum / sites >= 4000);             // the chain leans, metres not millimetres
    CHECK(realDriftSum / sites >= plainDriftMax * 3);
    CHECK(realHolesTotal >= plainHolesTotal * 8 + 8); // pillars, not pinched-off noise
    CHECK(sitesWithPillars * 2 >= sites);             // and most sites have them
    CHECK(realFloorSum / sites > 30);                 // floors are rubble, not planes
    CHECK(anisoBothWays * 2 >= sites);                // per site, not just on average
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
