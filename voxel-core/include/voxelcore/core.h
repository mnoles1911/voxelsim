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
// --- v23: CAP THE SUM OF THE TWO DETAIL POOLS, NOT EACH ONE SEPARATELY ------
//
// Everything above this line describes v22 (the savanna gate). v23 is a
// different change in a different file and is recorded here because the
// constant below is the only place a reader looks.
//
// The bake ships a heightfield that drains BY CONTRACT -- zero interior sinks
// -- and the client's detail band was putting the pits back. Measured on an
// alpine carrier: 0 interior sinks -> 1625, 0.0% stranded -> 87.9%, mean flow
// path 224 m -> 29 m.
//
// The cause was the SPLIT, not the multiplier. v19 divided the detail band into
// a routing pool and a micro pool, capped each against the carrier gradient
// SEPARATELY, and then summed them -- so the TOTAL could reach 2.2x the carrier
// gradient while each pool was individually "compliant". That defeats the
// guarantee task #21 exists for: detail must never be able to reverse the
// carrier's downhill. v23 caps the sum.
//
// The obvious move -- tightening the micro multiplier -- was the WRONG fix, and
// task #47 had attributed the regression to exactly that. See commit 0609be1.
//
// STILL OPEN after v23 (tasks #47/#48): the 11-window paired corpus was never
// re-run, and v23 has never been judged for visible terracing, which its own
// commit message demands.
//
// --- v24: THE ASSET TERM BECOMES INSTALLABLE WORLDGEN INPUT -----------------
//
// Everything above describes the terrain function, which v24 does not touch:
// a world with no AssetField installed is bit-identical to v23 (verified: the
// terrain-only digest is e02458de2be47309 before and after). What v24 adds is
// the third term -- 826 species from asset-forge's manifest, VXA v3 banks,
// and the four-layer scatter table -- composed into makeBrick/materialAt when
// a host installs it. FROM HERE ON, the manifest bytes, the bank bytes and
// the layer table are worldgen input: any change to any of them (a weight, a
// cap, a re-baked seed, a new seed file in a bank directory) changes voxels
// and is a version bump with goldens re-blessed, exactly like an amplifier
// constant. docs/asset-placement-architecture.md section 9 is the contract;
// vxc_bench --assets is the ran-flag (its digest MUST differ from the
// terrain-only one, and did: 3b5fe7ec61c6581a at radius 8, seed 20260719,
// brick 16, against the 2026-08-15 manifest and 4-seed banks).
//
// --- v25: THE FIRST TIME THAT CONTRACT WAS CALLED IN -----------------------
//
// v24 declared manifest bytes, bank bytes and the layer table to be worldgen
// input. On 2026-08-16 all three moved and this is the bump that says so. What
// changed is in asset-forge, not here:
//
//   * trunk taper (a real missing term -- an unforked bole kept its base radius
//     to the crown, and a birch measured 1.00), so every tree in the 688-file
//     bank is a different tree;
//   * quad.eye's float bounds made spec validation NON-IDEMPOTENT, reseeding
//     192 specs -- 9 trees and 34 rocks among them, all terrain kinds with
//     banks;
//   * 25 quadrupeds moved off the 5 cm lattice. They are detail kinds and have
//     no banks, but they are in the manifest.
//
// TERRAIN IS UNTOUCHED AND THAT IS VERIFIED, NOT ASSUMED: the terrain-only
// digest is e02458de2be47309 before and after, identical to v23's and v24's.
// Only the asset term moved: vxc_bench --assets goes 3b5fe7ec61c6581a ->
// 41ec6bbf103f18dc at radius 8, seed 20260719, brick 16.
//
// The staleness that made this bump necessary was found by asset-forge's
// tools/enginecheck.py, which compares each baked bank against its spec's
// spec_hash. Before it existed the exporter skipped a bake whenever the FILE
// EXISTED, so the engine would have gone on composing pre-taper trees forever
// with nothing to say so.
//
// --- v26: PLACEMENT READS THE GROUND'S OWN CHANNELS ------------------------
//
// The terrain function is untouched (terrain-only digest e02458de2be47309
// before and after -- assets are not terrain); what moved is which species
// stands where, in five ways:
//
//   * STANDING WATER finally reaches the veto: AssetColumnFacts gets its
//     water surface from the SAME composed lake/river datum the renderer
//     draws (assetchannels.h over IWaterSampler), plus the sea composed from
//     the column itself -- not from ColumnSample::waterSurfaceMm, the debug
//     marker that is empty in every production run and made the first veto
//     inert while the owner photographed oaks standing in the alpine lake.
//   * DISTANCE TO WATER is served from the fine tiles' new SECTION_PLACE_*
//     planes (bake_ver 28), un-refusing the 112 authored riparian species
//     that failed closed since the gate existed.
//   * The hard slope ceiling became a RESPONSE CURVE, sized inversely from
//     species height at import (tall crowns taper out by ~60% slope,
//     krummholz holds to 100%), and the treeline became a BAND: tall species
//     thin through the last 150-300 m below the temperature-adjusted line,
//     shifted by aspect heat load.
//   * TWI moisture, talus flux and curvature modulate pick weights through
//     per-species affinities DERIVED at import from what authors already
//     said (water_max, biome weights, kind, height) -- zero spec edits.
//   * Every channel is sentinelled: a world with no fine tiles or pre-28
//     tiles modulates by exactly 1 everywhere except the slope curve and the
//     treeline band, which need no channel.
//
// assetpolicy.h/assetfield.h/assetchannels.h/assetmanifest.cpp are the
// mechanism; vxc_bench --assets is the ran-flag, and its digest moves while
// the terrain-only digest must not. Verified at the bump (radius 8, seed
// 20260719, brick 16, 2026-08-17 manifest): terrain-only e02458de2be47309
// unchanged; --assets 41ec6bbf103f18dc -> ea75a87ab98e8cba.
//
// --- v27: BIOME REBALANCE (owner decisions, 2026-08-18) --------------------
//
// Two changes, both in biome.h's classifyBiome and BOTH terrain (surfaceMat
// moves, so unlike v24..v26 the digests MOVE at this bump):
//
//   * THE DRY-LAND CLIFF GATE IS GONE. "Bare rock should not really be a
//     thing at all unless it's underneath a water body or ocean." Slope >
//     kBiomeCliffSlopeMmPerM now classifies BARE_ROCK only BELOW the coastal
//     band (steep ocean floor: rock, not mud -- sediment does not rest above
//     the angle of repose); on dry land the column falls through to the
//     treeline/Whittaker answer, so steep hillsides carry their climate's
//     biome and its slope-curve-gated vegetation. Cliff FACES still show
//     rock: the topsoil retention floor (unchanged, still tied to the same
//     constant) leaves a one-voxel soil skin whose wall faces expose the
//     MAT_ROCK stratigraphy beneath.
//
//   * THE GRASSLAND/FOREST PRECIPITATION BOUNDARY MOVED 800 -> 450 mm/yr
//     (kBiomePrecipSemiU8, u8 17 -> 10) to trade grassland for temperate
//     forest on this world's compressed precipitation tails. Coarse-corpus
//     census (vxc_climateprobe 64/axis): TEMPERATE_FOREST 5.44% -> 9.67% of
//     world (11.9% -> 21.1% of land), GRASSLAND 14.38% -> 12.87%
//     (31.4% -> 28.1%), BARE_ROCK 5.25% -> 1.07% (all submarine, 0.00% of
//     land), TUNDRA_ALPINE 7.23% -> 9.03% (ex-cliff columns above the
//     treeline are alpine now), DESERT/SAVANNA/TAIGA/RAINFOREST/BEACH/OCEAN
//     within 0.25 points. The full reasoning, including why 450 is this
//     lever's floor and what it deliberately trades, is at the constants in
//     biome.h.
//
// NO TILE RE-BAKE IS NEEDED to see the new biomes: tiles carry raw climate
// channels (coarse) and elevation/water/placement planes (fine), never a
// biome byte -- classification is code, applied per column at generation
// time, so the next session over the SAME tiles renders v27 biomes. (The
// placement planes' talus threshold cites the cliff constant only as prose;
// the bake never calls classifyBiome.) What DOES go stale is anything
// derived from the old classification outside the tiles: census overlays,
// biome-keyed capture site lists, and cached session state.
//
// --- v28: PER-BIOME PLACEMENT (owner directive, 2026-08-18) ----------------
//
// The manifest moves to VXM2 (forge/manifest.py / assetmanifest.h) and the
// placement policy learns three per-biome facts. Terrain is untouched
// (terrain-only digest ad9c4c2a100b5a28 before and after -- assets are not
// terrain); what moves is which species stands where and HOW MANY:
//
//   * A KIND x BIOME DENSITY TABLE (rules/biome-density.json) scales the
//     occupancy test per asset class per biome, AFTER the species pick, so
//     it is linear through weight saturation -- the measured defect it
//     fixes: biome weights saturate the occupancy cap, so savanna carried
//     the same canopy density as temperate forest (277 vs 283 sites, audit
//     4.2) and only the global layer density acted anywhere. Values above
//     1000 per-mille are REFUSED: the table may only thin (veto-only), so
//     every streaming-bound argument stands unmodified.
//   * AN EXPLICIT BIOME ALLOWLIST per species (spec `biome_allow`, enforced
//     at export by zeroing outside weights, reported by name) -- "allowed in
//     one, many, or NO biomes" is now an auditable statement instead of an
//     implicit pattern of zeros.
//   * NAMED PLACEMENT RULES (rules/placement-rules.json), authored once and
//     ATTACHED per (species, biome), one or many; composition is
//     intersection (strictest wins), and the import splits each overridden
//     (species, biome) into its own gate row so the resolver stays scalar
//     compares. The owner's contract: "temperate type tree is almost
//     unrestricted in temperate forest but faces strict placement rules in
//     deserts such as must be near fresh water body."
//
// THE FORMAT LANDED AS A PROVEN NO-OP FIRST: with the density table neutral
// (all 1000) and no rules attached, the VXM2 species block is byte-identical
// to the VXM1 export, vxc_assetprobe's census is identical at three sites
// (savanna 35,829 / temperate_forest 33,029 / rainforest 35,568 instances,
// zero counters moved), and the composed-asset digest is unchanged
// (d6357c3b035c8c22 with old code + VXM1 and new code + neutral VXM2). The
// REAL per-biome densities then land as their own commit so the tuning diff
// is exactly the tuning -- both steps inside this one version bump, because
// no world shipped against the interim neutral manifest. With the tuned
// table (rules/biome-density.json) the composed-asset digest moves
// d6357c3b035c8c22 -> 48b159faa28a5e84 (the ran-flag), terrain-only stays
// ad9c4c2a100b5a28, and the census delivers the directive's contrast:
// savanna 143 -> 25 trees/ha with its grass untouched at ~3,200/ha, while
// taiga keeps 279 trees/ha -- cross-biome tree spread 5x -> 15.5x, grass
// 2.1x -> 10x (per-site tables in the tuning commit).
inline constexpr uint32_t kWorldGenVersion = 28;

inline constexpr int32_t kVoxelSizeMm = 100; // 10 cm voxels

// SEA LEVEL, and it is a symbol because it was fourteen literals.
//
// The datum has always been zero, and until now that was the entire
// specification: a trailing comment on kVoxelSizeMm above, plus a bare `0`
// wherever anything needed it. An audit for docs/watershed-system-plan.md
// item 1 found **21 unnamed literal-0 sea-level tests across five languages**
// (C++ in voxel-core and ue-project, HLSL, Python, and JavaScript embedded in
// a Python tool), three separate *named* sea-level symbols that did not agree
// on units (kRiverSeaLevelMm in mm -- and dead, zero references anywhere --
// rivercouple's mutable seaLevelVz in voxel z, caves.h's kCaveMinVoxelZ in
// voxel z), and one consumer drawing every published world map three metres
// below the game's waterline (terrain-service/tools/world_map.py). None of
// that is findable by grepping for `0`.
//
// So: ONE symbol, in mm because that is the unit every elevation on the wire
// and in the tile format uses, with a voxel-lattice twin derived from it
// rather than written down twice. Everything that means "sea level" now says
// so by name, and a change of datum is one edit rather than an archaeology
// project.
//
// It is NOT expected to move -- the whole world model is built on z=0 being
// the sea -- but "unlikely to change" was exactly the argument that produced
// the twenty-one literals.
inline constexpr int32_t kSeaLevelMm = 0;

// The same datum on the voxel lattice. Exact by construction: the static
// assert below refuses a sea level that is not a whole number of voxels,
// because a fractional one would make the two spellings disagree silently.
inline constexpr int64_t kSeaLevelVoxelZ = kSeaLevelMm / kVoxelSizeMm;
static_assert(kSeaLevelVoxelZ * kVoxelSizeMm == kSeaLevelMm,
              "kSeaLevelMm must be a whole number of voxels");

// --- bound decline sentinels ------------------------------------------------
//
// Returned by the surface/solidity bounds (Amplifier::surfaceUpperBoundMm,
// surfaceLowerBoundMm, solidBelowBoundMm -- see voxelcore/amplifier.h for the
// derivations) when they will not bound a footprint.
//
// INT64_MAX rather than a bool-out-param so a caller that forgets to check
// still gets the SAFE answer ("the terrain might reach arbitrarily high here"),
// never a false all-air verdict. Getting that backwards is not a lost
// optimisation: the streaming layer skips generating any chunk these prove
// empty, so a bound that is too tight is terrain that never generates.
inline constexpr int64_t kSurfaceBoundDeclined = INT64_MAX;

// The mirror sentinel, for the LOWER bound and the all-solid bound. INT64_MIN
// for exactly the same reason: it is the safe answer. A caller that forgets to
// check gets "the terrain might reach arbitrarily low here" / "nothing is
// provably solid here", never a false all-solid verdict. The two sentinels are
// deliberately different values so a caller cannot pass one where the other is
// meant and still compile into something that looks like it works.
inline constexpr int64_t kSurfaceLowerBoundDeclined = INT64_MIN;

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
    // DEBUG INSTRUMENT, not world content. Solid magenta voxels standing where
    // the bake says water is, so the water model can be judged at full clipmap
    // range instead of through the near-field renderer's 25.6 m horizontal /
    // 12.8 m vertical bubble. Off unless -VoxelWaterMarker=1.
    //
    // It is a MATERIAL rather than a render mode because that is what makes it
    // free: solid voxels go down the terrain path and inherit view distance,
    // the LOD chain, the greedy mesher and the capture harness with no new
    // machinery. It is also the one thing in this enum that is SOLID ABOVE THE
    // SURFACE -- see Amplifier::surfaceUpperBoundMm, whose soundness argument
    // that argument depends on.
    MAT_WATERMARK = 15,

    // --- ASSET MATERIALS ---------------------------------------------------
    // Wood and foliage, for the baked environment assets (asset-forge). The
    // terrain generator never produces any of these; they arrive only from an
    // asset, which is why they sit after everything the amplifier can emit.
    //
    // They are here because without them a baked tree renders as SOMETHING
    // rather than failing. MaterialId is a uint8_t, so an asset built from id
    // 20 does not fault -- it indexes past the end of every material-keyed
    // array in the engine and the mesher cheerfully emits quads for it. The
    // whole library except rock, grass and reeds was in that state: nine of
    // the thirteen materials in use were past the end of this enum.
    //
    // The numbers are not free choices. asset-forge has been baking these ids
    // since before the engine had them (forge/materials.py), so every .vxa
    // already on disk carries them and the two lists have to agree exactly.
    MAT_BARK = 16,           // generic tree bark
    MAT_HEARTWOOD = 17,      // inner wood, exposed on cut or broken trunks
    MAT_DEADWOOD = 18,       // standing dead wood, snags, desert skeletons
    MAT_LEAF_BROADLEAF = 19, // temperate broadleaf foliage
    MAT_LEAF_NEEDLE = 20,    // conifer needles
    MAT_LEAF_JUNGLE = 21,    // rainforest foliage, palm fronds
    MAT_LEAF_DRY = 22,       // savanna and arid foliage
    MAT_BARK_PALE = 23,      // birch and aspen: white/silver bark
    MAT_LEAF_BLOSSOM = 24,   // cherry and other flowering species
    MAT_LEAF_AUTUMN = 25,    // seasonal turn

    // --- CREATURE MATERIALS ------------------------------------------------
    // Skin colours for the environment animals (asset-forge `fish` kind, and
    // whatever animals follow it). Asset materials in every sense that matters
    // to the renderer -- the terrain amplifier never emits one -- so they sit
    // after the ASSET MATERIALS banner rather than getting a second one that
    // `gen_material_palette_ush.py` would have to learn about.
    //
    // NAMED FOR WHAT THEY LOOK LIKE, NOT FOR WHAT WEARS THEM. Fish are the
    // first animal here and will not be the last: a frog is MAT_SKIN_GREEN, a
    // trout's back is MAT_SKIN_OLIVE, and a clownfish and a goldfish are both
    // MAT_SKIN_ORANGE. Ten entries because authoring the first ten species
    // with fewer did not work -- six terrain materials come close enough to be
    // tempting and each is wrong in a way that shows on a twelve-voxel flank.
    // See asset-forge/docs/fish-colour-proposal.md.
    //
    // The appearance numbers for these sit at the QUIET end of every range in
    // materialpalette.h, which is the opposite end from foliage and gravel. An
    // animal is one smooth creature, not a granular surface; at foliage's
    // jitter of 58/255 a flank reads as television static.
    MAT_SKIN_DARK = 26,      // near-black: eyes, stripes, bars, the top of a back
    MAT_SKIN_PALE = 27,      // off-white belly, the pale half of countershading
    MAT_SKIN_SILVER = 28,    // bright silver flank; the commonest fish colour there is
    MAT_SKIN_OLIVE = 29,     // olive-green back: trout, perch, pike
    MAT_SKIN_BROWN = 30,     // warm brown: bottom fish, speckled backs
    MAT_SKIN_ORANGE = 31,    // clownfish, goldfish, koi
    MAT_SKIN_YELLOW = 32,    // reef yellow
    MAT_SKIN_RED = 33,       // red fins, rudd, snapper
    MAT_SKIN_BLUE = 34,      // reef blue and the blue-green of open water
    MAT_SKIN_GREEN = 35,     // bright green: wrasse, weed-bed fish

    // --- PLUMAGE MATERIALS -------------------------------------------------
    // Feather and bill colours for the asset-forge `bird` kind. Asset
    // materials in every sense the renderer cares about, exactly as the
    // creature skins above are, so they stay under the ASSET MATERIALS banner
    // rather than getting one of their own that
    // `gen_material_palette_ush.py` would have to learn about. The terrain
    // amplifier never emits one.
    //
    // ELEVEN, AND THE REASON IS A MEASUREMENT RATHER THAN A TASTE. Delhey 2015
    // took 46,559 reflectance spectra over 555 species and found melanin is
    // 74% of a bird's plumage AREA and 7% of the colour GAMUT, while
    // structural colour is 7% of the area and 45% of the gamut. So a palette
    // weighted the way a field guide is weighted comes out browns and greys,
    // and a raven, a pigeon and a sparrow read as three grey-brown lozenges.
    // A stylised library has to over-weight the rare colours deliberately.
    //
    // The twenty species in the library fill 160 material slots between them:
    // 92 of those are one of these eleven and 68 are one of the ten skins
    // above, which is why the ask was eleven and not twenty-five. Four of
    // these are NEUTRALS the fish set has no equivalent for (a fish belly is
    // cream, not white; the one grey in the skin set is the mirror-flanked
    // silver; there is no dark cool grey and no sandy tan anywhere in the
    // palette), six are SATURATED HUES, and one is keratin.
    //
    // THE BILL IS ITS OWN MATERIAL BECAUSE IT IS THE CHEAPEST IDENTIFYING
    // FEATURE A BIRD HAS. Pixel Logic records that Super Mario World's
    // Swoopers are bats that read as birds purely because their nose was
    // coloured orange -- an orange protrusion in the head position converts a
    // bat into a bird. Yellow and orange bills use MAT_SKIN_YELLOW and
    // MAT_SKIN_ORANGE, so only the grey one is new.
    //
    // See asset-forge/docs/bird-colour-proposal.md for the per-entry argument
    // and the contrast numbers each one was chosen against.
    MAT_PLUME_WHITE = 36,    // true white: gull, egret, ptarmigan, wing bars
    MAT_PLUME_GREY = 37,     // neutral mid grey: pigeon, gull mantle, tit
    MAT_PLUME_SLATE = 38,    // dark cool grey: heron, jay wing, falcon back
    MAT_PLUME_BUFF = 39,     // sandy tan: lark, gamebird, hoopoe, female duck
    MAT_PLUME_RUFOUS = 40,   // chestnut: robin breast, kestrel back, jay body
    MAT_PLUME_CRIMSON = 41,  // bright scarlet: macaw, woodpecker nape
    MAT_PLUME_LIME = 42,     // yellow-green: parrot, greenfinch, bee-eater
    MAT_PLUME_CYAN = 43,     // electric turquoise: kingfisher, roller, macaw
    MAT_PLUME_LILAC = 44,    // violet: jay covert, pigeon and starling gloss
    MAT_PLUME_IRIDESCENT = 45, // saturated dark teal: starling, mallard head
    MAT_BEAK_HORN = 46,      // grey keratin: bills, legs and feet
    kMaterialCount
};

// Floored division/modulo (int division in C++ truncates toward zero; world
// coordinate -> lattice/pixel/brick index must floor instead).
constexpr int64_t floorDiv(int64_t a, int64_t b) {
    const int64_t q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
constexpr int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }

// THE TOPMOST SOLID VOXEL OF A COLUMN, from its surface elevation. A voxel is
// solid when its CENTRE is at or below the surface, so this is the index whose
// centre is the highest one that still qualifies.
//
// ONE RULE, TWO CALLERS, AND THAT IS THE POINT OF IT BEING HERE. This formula
// was written out inline in GeneratedWorld::surfaceBrickRange, which decides
// which bricks a chunk materialises, and asset placement needs the identical
// answer to decide which voxel a tree stands on. A tree anchored by a second,
// separately-derived version of this arithmetic would sit one voxel off
// wherever the two disagreed -- which is a floating tree, or a buried one, and
// neither is logged. Two derivations of one rule is how the cavern reach and
// the carrier stencil came to disagree once already (tilestreaming.h:190-197).
//
// IT IS NOT A SOLIDITY TEST. It answers "where the surface puts the top voxel",
// and the carve passes (caves, caverns) can make that voxel AIR -- a cave mouth
// or a sinkhole shaft is exactly that. Anything anchoring to it must then ask
// Amplifier::materialAt whether the voxel is really there. See
// AssetColumnFacts::anchorSolid.
constexpr int64_t topSolidVoxelZ(int64_t surfaceMm) {
    return floorDiv(surfaceMm - int64_t(kVoxelSizeMm) / 2, int64_t(kVoxelSizeMm));
}

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
