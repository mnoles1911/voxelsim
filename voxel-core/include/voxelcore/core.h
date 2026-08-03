#pragma once
// voxel-core: engine-agnostic voxel world core. UE-header-free by doctrine.
// Determinism conventions: docs/determinism.md. No floats in world derivation.

#include <cstdint>

namespace vxc {

// Bumped on any deliberate change to worldgen math (hash, octave tables,
// stratigraphy constants). Invalidates edit logs and golden digests.
// v3: SyntheticTileSampler spectral-gap fill â€” 4 new elevation octaves at
// 480/240/120/60 m wavelength (70/38/20/11 m amplitude), terrain-realism audit.
// v4: M4 cave pass (voxelcore/caves.h) â€” jittered-lattice tunnel network
// carved into the voxelize path. Underground is no longer uniformly solid.
// v5: M4 cave pass v2 â€” (a) the cavern system (voxelcore/caverns.h) folded
// into ColumnSample/materialAt alongside caves, and (b) the bedrock top moved
// from a 40-60 m band to a 180-220 m one (200 m mean, Matt's decision), which
// is what gives the multi-storey cavern chains their vertical room. One bump
// covers both; see docs/status.md "C4".
// v6: coarse-to-fine detail rework, the first time the amplifier was measured
// against REAL 30 m terrain-diffusion tiles rather than SyntheticTileSampler.
// Three changes, all in the surface term: (a) detail octave table v2 â€” five
// octaves down to a 200 mm lattice, split into a slope-scaled LANDFORM band
// and a microrelief band whose scale has a floor so it does not vanish on flat
// ground; (b) the detail octaves now use the quintic-faded value noise
// (hash.h valueNoise2Fade) instead of the raw bilinear one, which removes the
// dead-straight lattice creases; (c) the surface bounds widened to match.
// See amplifier.cpp kDetailOctaves for the measurements that forced this.
// v8: the CLIMATE half of what v6 did for the surface half -- the first time
// the biome and stratigraphy consumers were measured against real tiles rather
// than SyntheticTileSampler. voxelcore/climate.h now defines the wire encoding
// (physical WorldClim units, mirroring terrain-service's EXPECTED_CHANNELS),
// every biome threshold is derived from it at compile time instead of being a
// bare u8 literal, SyntheticTileSampler emits that same encoding (and finally
// fills seasonality/precipVariability), classifyBiome tests sea level before
// slope, BARE_ROCK is appended, and the topsoil formula erodes by a FRACTION
// rather than subtracting an absolute depth. Measured with vxc_climateprobe;
// see docs/status.md for the before/after census.
//
// NOTE ON THE SKIPPED v7: the unmerged branch claude/erosion-v7 already claims
// 7 (drainage carving + a region-fitted precip retune). This work branched from
// main at v6 and takes 8 so the two can land in either order without colliding.
// The check at editlog.h is exact equality, not a range, so a gap is harmless.
// v9 (docs/terrain-amplification-plan.md Phase 1): the C2 carrier. The tile
// base is a uniform cubic B-spline instead of bilinear, killing the gradient
// step at every tile-pixel boundary that made the 30 m grid visible under
// directional light; the detail/soil/biome slope currency moves from mm per
// tile PIXEL to mm per METRE, taken from the carrier's analytic gradient, which
// both removes the per-cell gain step and fixes the scale-dependence biome.h
// had recorded as a latent bug; climate is sampled with a faded bilinear plus
// an ecotone dither instead of nearest-pixel, so material boundaries stop
// snapping to the pixel grid. surfaceBoundsMm is re-derived as a Lipschitz
// bound around one carrier evaluation.
// v10 (docs/terrain-amplification-plan.md Phase 3): structured detail. The
// sub-30 m band stops being isotropic fBm draped over a ramp and becomes
// conditioned on the carrier's own geometry.
//
// Four changes, and the reason each exists is a MEASUREMENT on v9, not a taste:
//   * a curvature gate on the shaping band -- crests roughen, hollows smooth
//     toward colluvial fill. v9's curvature-conditioned roughness measured
//     0.98-1.03, i.e. exactly the 1.0 a stationary fBm gives: its detail was
//     conditioned on nothing at all;
//   * a rill/flute term, anisotropic in the local gradient frame. v9's
//     across/along-slope roughness ratio measured 0.98-1.01 against a 45 deg
//     control at 1.00-1.02, on grades up to 50% -- water cuts downslope, and
//     nothing in v9 knew which way that was;
//   * a bedding term on a regional 819.2 m strike/dip field, so layered rock
//     reads as layered over a whole hillside rather than per-point;
//   * BAND OWNERSHIP: on a world whose tiles carry the baked 1.875 m fine tier,
//     the 25.6 m and 6.4 m landform octaves are deleted and the ladder is
//     continued from 3.2 m instead. Synthesising them over measured landform
//     does not add detail, it fights the bake with hash noise.
//
// The surface bound is re-derived for all of it and TIGHTENS by 3.5x on a fine
// world (7846 mm against 27439 mm at maximum slope and curvature).
// v12 (docs/terrain-amplification-plan.md Phase 3d/4): the bounded 3D density
// band. `Amplifier::stratigraphyAt` stops testing `voxel centre <= surface` and
// tests `centre <= surface + D(x, y, z)` instead, with a compile-time envelope
// |D| <= 350 mm (voxelcore/density3.h). This is the first time in this
// codebase's history that the solid set is not the region under a graph: a
// column can read air with solid above it, i.e. the world can have overhangs.
//
// D is the SAME regional strike/dip bedding field the 2D banding already uses,
// seen volumetrically and put through a monotone odd contrast curve (the tent
// profile alone produced overhangs on 0.098% of gated columns -- bounded,
// continuous, gated and geometrically inert), plus a rock-gated 3D pocket term.
// Both gates are cheap and hoisted per column, and outside the +/-350 mm band D
// cannot flip the test, so the skip is EXACT rather than approximate -- which
// is the only reason a per-voxel hashing pass is affordable at all.
//
// Everything derived from the surface widens by that one constant:
// surfaceUpperBoundMm, surfaceLowerBoundMm and (through it) solidBelowBoundMm,
// plus GeneratedWorld's surface brick range -- the latter PER COLUMN, since D is
// identically zero wherever the gates are shut. The air-reason enumeration in
// amplifier.h gains its fourth reason.
//
// THE ENVELOPE IS 350 mm AND NOT THE PLAN'S 700, and that is the whole story of
// this version. At 700 the term was implemented, wired in and MEASURED: 2.6x
// world-average voxelisation on real diffusion tiles and +13% bricks, for an
// overhang on 0.22% of all columns. It was not merged. At 350 the band halves,
// the pocket contributor is deleted outright (it produced no overhangs at any
// amplitude and was the dominant cost), the contrast curve goes to three passes
// to recover the rate the smaller amplitude cost, and the lithology gate is
// promoted to the whole displacement so the term is live on bare rock only.
// --- v13: the client stopped destroying the bake on gentle ground -----------
//
// Two changes, both in the coarse-to-fine amplification path, both driven by
// docs/terrain-validation-2026-07.md rather than by taste.
//
//   1. THE COARSE CARRIER PREFILTERS ITS CONTROL LATTICE (carrier.h). A cubic
//      B-spline approximates its control points, so feeding 30 m raster SAMPLES
//      in as control points low-passed the diffusion model's own output by
//      815 mm on plains and 3222 mm on alpine before any amplification ran. The
//      baked fine tier ships prefiltered control points and never had this; the
//      30 m tier had no such step until now.
//
//   2. DETAIL AMPLITUDE IS CONDITIONED ON LANDFORM RELIEF, NOT ON GRADIENT
//      (carrier.h reliefScaleQ10, replacing slopeScaleQ10 and microScaleQ10).
//      The old gates spanned 1.9x and 1.06x between a plain and a mountain that
//      differ 25x in relief, so the detail pass laid down the same ~0.3-0.6 m of
//      roughness on both: +207% of a plain's 1.875 m mean slope against +3.3% of
//      alpine's. The finished plain measured SIX TIMES more ridged than the
//      finished mountain (frac_ridge_peak 0.352 against 0.061, real plains
//      0.054-0.073). Relief is a second difference of the raster at a fixed 30 m
//      baseline: exactly zero on a plane at any grade, and a direct measurement
//      of what the tier carries rather than an assumption about it.
// --- v14: the detail ladder stops reversing the carrier's downhill -----------
//
// One change (amplifier.cpp, "THE DETAIL GRADIENT CAP"), driven by
// docs/measurements/client-detail-drainage-2026-07-29.txt: at v13 the post-gate
// detail ladder sums to ~2.3 of local gradient on ground whose own gradient is
// 0.4, so the downhill direction reversed almost everywhere and the client
// turned the bake's routed drainage into a field of closed decimetre basins
// (alpine: carrier 0 interior sinks -> amplified 1625; 87.9% of a 40% slope
// stranded). v14 scales the summed post-gate detail down, in one joint factor,
// so its nominal gradient stays within max(gradFloor, k * carrierGradient).
// The relative octave weighting is untouched; the floor keeps decimetre
// roughness alive on flats (the anti-terrace band). Scale-down only, so every
// surface bound derived at v13 is still sound unchanged.
// --- v20: the 3D density band is removed -------------------------------------
//
// v12's bounded 3D density band (density3.h) is no longer applied.
// Amplifier::stratigraphyAt tests `centre <= surfaceMm` again, and
// Amplifier::column leaves ColumnSample::d3 default-constructed.
//
// WHY. Measured on 2026-07-31 as the source of the parallel-band artifact this
// project has been chasing since it started
// (docs/measurements/placed-voxel-banding-2026-07-31.txt). The displacement
// moved the top solid voxel on 88.61% of gated columns, and an FFT of it puts
// 60% of the variance at 0.8-3.2 m -- detail_bedding.h's bed-thickness range.
// It went unfound for so long because every instrument in the tree measured
// amp.surfaceMm, and this term displaces voxel PLACEMENT off that surface by up
// to 3.5 voxels; the fix included a new probe that reads placed voxels
// (terrainprobe.cpp's VXC_PROBE_DUMP_VOXEL).
//
// AND IT COULD NOT BE TUNED. An overhang needs dD/dz > 1, which needs the bed
// contacts sharpened, which is exactly what makes the ledges hard and parallel:
// density3.h's own sweep gives 0.01% overhangs raw, 0.54% at two contrast
// passes, 1.97% at three (shipped), 3.57% at four. Every setting is either
// banded or geometrically inert. The trade it was making: an overhang on about
// 0.13% of world columns, paid for by banding ~90% of every steep rock face.
//
// KEPT: the 2D bedding term (kBeddingAmpMm = 120, in evalSurface). It is part
// of surfaceMm and was present in the clean panel of the A/B the owner
// approved.
//
// Bounds are UNCHANGED at v20 and stay widened by kDensity3MaxAbsMm. That is
// conservative, never unsound, and the tightening is a separate commit so the
// geometry change and the bound change can be reviewed and reverted apart.
// --- v21: the micro gradient cap comes down, 1.5 -> 1.2 ---------------------
//
// One constant, kMicroGradCapKQ10 in amplifier.cpp, where the derivation and
// both sweeps live. Summary of why it moved:
//
// v19 raised the micro pool's cap above 1.0 to break voxel-quantisation bands,
// and chose 1.5 over 1.2 while the owner's rejected banding was still
// unexplained -- so band width was being bought at whatever drainage it cost.
// v20 identified that banding as the 3D density band, which no setting of this
// constant could ever have affected. With the confound gone the trade is
// honest, and a fresh sweep says 1.5 was paying too much:
//
//     k=0.5    1 sink    0.4% stranded      k=1.2    9 sinks   1.2% stranded
//     k=1.0    5 sinks   0.4% stranded      k=1.5   17 sinks   3.5% stranded
//
// 1.2 halves the sinks and cuts stranded area threefold against 1.5, for a band
// width of 0.20 m against 0.10 -- nine times better than the 1.82 m an uncapped
// micro pool leaves on that ground. The measurement also killed the idea of
// reverting to v14's 0.5: the drainage knee is at 1.0, and below it there is no
// stranding left to recover.
//
// THIS IS THE ONLY SURVIVING v19 CLIENT CHANGE. Its other two -- the meso
// octaves and the second carrier-warp component -- were already withdrawn when
// the meso band moved into the bake as B4.
// --- v22: the savanna gate changes CHANNEL, bio_4 -> bio_15 ------------------
//
// One gate in biome.h, where the derivation lives. Summary of why it moved:
//
// SAVANNA was UNREACHABLE from v8 to v21 -- not rare, impossible. The gate was
// `bio_1 >= 18 C && bio_4 >= 1500` (temperature seasonality, sd of monthly
// temperature >= 15 C). Those two conditions do not co-occur on Earth and
// nearly cannot: the warm band IS the tropics, and what makes a place tropical
// is that its temperature barely moves through the year. Measured over the
// WorldClim 2.1 10' rasters inside the same +/-60 deg crop the conditioning
// stats use, the maximum bio_4 anywhere with bio_1 >= 18 C is 1084 against a
// threshold of 1500 -- ZERO pixels of Earth land, so MAT_SAVANNA_GRASS has
// never been placed and every warm semi-arid column fell through to GRASSLAND.
//
// AND NO bio_4 THRESHOLD FIXES IT. Sweeping it down to a value that admits a
// plausible share selects the wrong places: bio_4 >= 200 takes 12.5% of land
// but only 77.0% of it lies inside |lat| < 25, against 87.2% for the eligible
// window itself -- the rule is ANTI-selective for the tropics. It calls
// Houston, Brisbane, Miami, Durban and Seville savanna while rejecting the
// Serengeti, the Cerrado and Tsavo. Wrong variable, not wrong number.
//
// bio_15 (precipitation seasonality, the CV of monthly rainfall) is the
// variable a savanna actually differs by -- a DRY SEASON -- and it was already
// in the wire format and already uploaded to the GPU, so nothing new is
// plumbed; the shader blends bits 24-31 of the same packed word instead of bits
// 8-15. At CV >= 70% every in-window negative control above is rejected, 8 of 9
// savanna sites accepted, and 93.4% of the selected area is inside |lat| < 25.
//
// The threshold is derived twice and both routes give 70%: a year with d dry
// months has CV = sqrt(d/(12-d)), so four dry months is sqrt(4/8) = 70.7%; and
// 70% puts SAVANNA at 15.57% of Earth land against the ~15.6% it really covers.
//
// NOT CHANGED, DELIBERATELY: kBiomeTempHotU8 stays at 24 C even though the v21
// census showed DESERT at 0.00%. On Earth 24 C gives 9.50% desert, which is
// right; the census world is 16.7% arid but its land temperature p95 is 20.7 C,
// so the fault is the coarse model's compressed climate tails, not the gate.
// Lowering it would paint dry temperate land with sand. See
// docs/measurements/biome-gates-2026-08-01.txt.
//
// WIRE FORMAT UNCHANGED. All four climate bytes were already carried and
// blended; only which byte classifyBiome reads moved. Tiles do not need
// regenerating and provider_id does not roll for this.
//
// ---------------------------------------------------------------------------
// v24 -- WAYPOINTED CAVE PASSAGES AND VARIABLE CALIBRE
// (docs/underground-system-plan.md W2)
// ---------------------------------------------------------------------------
// The owner's complaint was that caves "look very computer made with procedural
// shapes". Two of the mechanical tells behind that are straight constant-radius
// capsules and a hard lattice direction. v24 addresses the first two:
//
//   * every lattice edge's axis becomes a two-segment polyline through one
//     hash-jittered interior waypoint (sideways up to 5.12 m, dipped up to
//     1.0 m), and each sub-segment reduces independently -- a waypointed edge
//     can pass the same column twice at two depths;
//   * the tube radius interpolates between three control values (each end
//     NODE's own calibre and the per-edge draw as the mid value) instead of
//     being one constant per edge, so passages swell and choke.
//
// Two new hash channels, 29 and 50 (hash.h's declared-free ids). Radius RANGE
// unchanged; the plan's widening to [0.8, 4.0] m belongs with the field
// coupling that decides where the wide end lives, and would break the crevice
// containment static_assert as it stands.
//
// Structure untouched: both endpoints are still lattice nodes, so the backbone
// connectivity proof, the depth-space roof guarantee, the shaft's
// bottoms-out-on-a-node argument and all three bedrock guards are unchanged.
// Measured connectivity did not regress (flat-terrain largest component
// 90.12% -> 90.48%, real terrain 65.01% -> 70.15%).
//
// WHAT IT DOES NOT DO, measured rather than assumed: it does not flatten the
// network's ROUTING. Tunnel axis length within 15 deg of a cardinal falls from
// 54.1% to 47.8%, but the net heading of eight consecutive backbone edges still
// departs from due E/N by only 2.1 deg on average -- because both ends of a run
// are still lattice nodes. vxc_caveprobe reports those as two separate numbers
// for exactly this reason. Moving the routing lock needs off-axis edges.
//
// TILES DO NOT NEED REGENERATING: caves are runtime worldgen only and the bake
// computes no underground data. Saved edit logs invalidate as usual.
inline constexpr uint32_t kWorldGenVersion = 25;

inline constexpr int32_t kVoxelSizeMm = 100; // 10 cm voxels; z=0 is sea level

using MaterialId = uint8_t;

// Material set v1 (amplifier stratigraphy + M4 per-biome surface materials,
// voxelcore/biome.h). Water is implicit (z<0 above terrain) and never stored
// in terrain bricks. IDs are append-only â€” never renumber an existing entry,
// it would invalidate every saved edit log.
enum Material : MaterialId {
    MAT_AIR = 0,
    MAT_BEDROCK = 1,       // deep unweathered rock, unbounded depth floor
    MAT_ROCK = 2,          // sedimentary layers between subsoil and bedrock
    MAT_GRAVEL = 3,        // coarse subsoil under sandy surfaces (beach/desert)
    MAT_SAND = 4,          // beach + desert surface
    MAT_SUBSOIL = 5,       // generic layer between topsoil and rock
    MAT_TOPSOIL = 6,       // generic fertile soil; temperate-forest surface
    MAT_SNOW = 7,          // legacy v0 high-altitude cap (superseded by
                            // MAT_PERMAFROST/MAT_ROCK for biome surfaces, kept
                            // stable for any existing saved edit logs)
    MAT_GRASS = 8,         // grassland surface
    MAT_JUNGLE_SOIL = 9,   // rainforest surface: dark, wet, organic-rich soil
    MAT_SAVANNA_GRASS = 10,// savanna surface: dry, seasonal grass
    MAT_PODZOL = 11,       // taiga surface: acidic boreal-forest soil
    MAT_PERMAFROST = 12,   // tundra/alpine surface: frozen ground
    MAT_MUD = 13,          // ocean floor / future wetland surface
    MAT_CLAY = 14,         // fine sediment; headroom for future
                            // floodplain/riverbank biomes (M4 rounds 2-3)
    kMaterialCount
};

// Floored division/modulo (int division in C++ truncates toward zero; world
// coordinate -> lattice/pixel/brick index must floor instead).
constexpr int64_t floorDiv(int64_t a, int64_t b) {
    const int64_t q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
constexpr int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }

constexpr int64_t clampi64(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
constexpr int32_t clampi32(int64_t v, int32_t lo, int32_t hi) {
    return static_cast<int32_t>(v < lo ? lo : (v > hi ? hi : v));
}

// FNV-1a 64 â€” determinism digests only (not worldgen randomness).
struct Digest {
    uint64_t h = 0xcbf29ce484222325ull;
    constexpr void u8(uint8_t v) { h = (h ^ v) * 0x100000001b3ull; }
    constexpr void u16(uint16_t v) { u8(static_cast<uint8_t>(v)); u8(static_cast<uint8_t>(v >> 8)); }
    constexpr void u32(uint32_t v) { u16(static_cast<uint16_t>(v)); u16(static_cast<uint16_t>(v >> 16)); }
    constexpr void u64(uint64_t v) { u32(static_cast<uint32_t>(v)); u32(static_cast<uint32_t>(v >> 32)); }
    constexpr void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
};

} // namespace vxc
