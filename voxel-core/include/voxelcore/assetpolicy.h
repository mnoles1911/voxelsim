#pragma once
// WHICH SPECIES STANDS AT A SITE, AND WHETHER ANYTHING DOES.
//
// assetplacement.h scatters anonymous SITES and stops there on purpose: "choosing
// a species needs biome, and the whole point of the split is that this half is
// computable without one" (assetplacement.h:147-151). This file is the other
// half -- the part that reads a column, and therefore the part that may not run
// on the streaming admission path.
//
// ---------------------------------------------------------------------------
// EVERYTHING IN THIS FILE IS A VETO, AND THAT IS NOT A STYLE CHOICE
// ---------------------------------------------------------------------------
//
// assetplacement.h's bound is sound over every policy because of one rule
// (assetplacement.h:58-62):
//
//     A POLICY MAY ONLY VETO A SITE. It may not create one, move one, or
//     substitute a taller asset than its layer's declared maximum.
//
// This file is that policy, so the rule is this file's contract. Every
// mechanism below removes sites from the set assetSiteInCell already produced
// and nothing adds to it:
//
//   biome weight zero      -> removes
//   elevation band         -> removes
//   slope band             -> removes
//   distance to water      -> removes
//   summed-weight occupancy-> removes
//   the cluster field      -> removes  (see THE CLUSTER FIELD below; it is the
//                                       one that looks like it might not)
//   ground anchoring       -> removes
//
// The species PICK looks like the exception and is not: a species may only be
// picked from the ones filed on the site's OWN layer, and every species is
// height-checked against its layer by assetLayerAdmitsHeight at load. So the
// tallest thing a pick can produce is the layer's maxHeightMm, which is what
// the bound already assumed. assetSpeciesTableIsWellFormed checks that filing
// at load rather than trusting it, and test_assetpolicy.cpp pins the direction.
//
// The bound cannot reach this file: assetplacement.h does not include it.
//
// ---------------------------------------------------------------------------
// THE ORDER OF OPERATIONS, and why the cheap gates run first
// ---------------------------------------------------------------------------
//
//   1. HABITAT SCAN. Walk the species filed on this site's layer. For each,
//      test biome weight, elevation band, slope band and distance to water,
//      and accumulate its pick weight. Integer compares only -- NO HASHES.
//   2. OCCUPANCY. If the accumulated weight is zero, nothing here grows: the
//      site is rejected. Otherwise the summed weight IS the probability the
//      site is used at all. One hash.
//   3. PICK. Weighted draw over the surviving species, from the same hash.
//   4. CLUSTER. The picked species' own grove field vetoes the site. Four
//      hashes (one faded value-noise sample).
//   5. GROUND. The instance is anchored to a ground sample the CALLER supplies,
//      and is refused if there is not one. See THE ANTI-FLOAT GUARD.
//
// Five hashes per surviving site, plus a linear scan of one layer's species
// list. Step 2 is what makes vegetation thin out in a desert without any
// per-biome density table: if the only tree eligible in a desert column is a
// saguaro at weight 0.1, then 10% of that layer's sites carry a saguaro and the
// other 90% carry nothing. A fixed layer density with a pick on top would have
// put a saguaro at EVERY site, which is a forest of cacti.
//
// ---------------------------------------------------------------------------
// THE CLUSTER FIELD, and the measurement that made it necessary
// ---------------------------------------------------------------------------
//
// The owner: "Rocks and trees follow their own algorithms and patterns for
// clustering and grouping according to slope and environment." placement.cluster
// is authored across the whole library and means it -- grass 0.35-1.0 (median
// 0.95), trees 0.05-0.95 (median 0.6), rocks 0.05-1.0 (median 0.35).
//
// THE USUAL ARGUMENT FOR CLUSTERING IS WRONG HERE, IN AN INTERESTING DIRECTION.
// It is normally stated as "a uniform scatter is Poisson and real stands are
// clumped". But assetSiteInCell is one site per lattice cell with a jitter
// INSIDE the cell -- a jittered grid, which is not Poisson, it is more regular
// than Poisson. Measured by the index of dispersion over quadrats (variance
// over mean; 1.0 is Poisson, above is clumped, below is regular), printed by
// assetcluster_measures_as_clumped_and_the_bare_scatter_measures_as_regular:
//
//     bare scatter at 400 per-mille   ~0.6    MORE REGULAR THAN RANDOM
//     clustered at strength 1.0       ~2.6    clumped
//
// A forest scattered the first way does not read as "trees placed at random",
// it reads as an orchard, and it reads as one MORE strongly than a Poisson
// scatter would. That is worth knowing before writing the code rather than
// after shipping it.
//
// THE MECHANISM IS A MEAN-PRESERVING DENSITY MODULATION, WHICH IS A VETO.
// A smooth field at a coarse lattice modulates the per-cell keep THRESHOLD:
//
//     gainQ10 = 1024 + clusterQ10 * fieldValue / 32768
//     keep iff  draw * 1024 < 1000 * gainQ10
//
// The field MODULATES the threshold rather than being thresholded itself, and
// that is the whole reason this shape was chosen. valueNoise2Fade's output is
// bell-shaped, not uniform, so thresholding it at "the 40% quantile of a
// uniform" would silently deliver something quite different from 40% of the
// sites -- a species shipping at a third of its authored abundance, which
// nothing would report. Modulating instead gives E[gain] = 1024 exactly
// (valueNoise2Fade has mean ~0), hence E[keep] = the unclustered rate,
// whatever the field's distribution is. Pinned by
// assetcluster_preserves_density_within_a_few_percent.
//
// EACH SPECIES GETS ITS OWN FIELD, via the seed rather than the channel: the
// channel is already spent per layer, and folding the species index into the
// seed gives an independent field per species for free. Without it every
// species in a biome would clump in exactly the same places, and the world
// would have fertile patches and bare patches rather than a beech grove beside
// an oak grove.
//
// WHAT THIS DOES NOT DO. It does not attempt "is there a cliff uphill of me",
// which is what scree really wants. That needs a second column evaluation at an
// offset -- affordable here, since this is not the admission path, but a
// different mechanism with a different cost. The slope BAND (slopeMinMmPerM,
// not just a maximum) gets most of the look for one integer compare: talus sits
// on ground steep enough to be talus and below the 70% grade at which
// classifyBiome returns BARE_ROCK, and a band says that where a ceiling cannot.

#include <cstdint>

#include "voxelcore/assetplacement.h"
#include "voxelcore/biome.h"
#include "voxelcore/core.h"
#include "voxelcore/hash.h"

namespace vxc {

// Cluster strength and abundance are carried as Q10 fixed point (1024 == 1.0)
// rather than as a per-mille, because they are MULTIPLIERS and per-mille reads
// as a probability. asset-forge authors both as a 0..1 float.
inline constexpr int32_t kAssetQ10One = 1024;

// The coarse lattice a layer's grove field is sampled on, as a multiple of the
// layer's own cell pitch.
//
// FOUR, and both neighbouring values are visibly wrong. At 1x the field
// decorrelates between adjacent cells and is indistinguishable from the white
// per-cell draw it is modulating -- clustering that changes nothing, which is
// this project's signature failure. At 16x a "grove" is 500 m across for a
// canopy layer, which is not a grove, it is a biome, and the biome gates
// already own that scale. At 4x a grove is a few cells across and a few tens
// of metres, which is what a stand is.
inline constexpr int64_t kAssetClusterLatticeCells = 4;

// One species, reduced to what placement needs. This is the C++ side of an
// asset-forge spec's `biomes.*` and `placement.*` groups; the importer that
// reads 828 JSON files and emits this table is asset-forge's to write (it is a
// bake step and that is where the schema lives).
//
// EVERY DISTANCE IS MM AND EVERY WEIGHT IS AN INTEGER. Placement is worldgen:
// same answer on every machine, and eventually in HLSL. voxel-core's CI bans
// float and double under include/ and src/ outright.
struct AssetSpecies {
    // Which decoded (species, seed) bank this draws its voxels from. Opaque
    // here -- this file never touches asset geometry.
    uint16_t bankId = 0;

    // Which AssetLayer this species is filed on. Chosen at bake time from
    // placement.spacing_m: the LATTICE is the spacing mechanism (a per-instance
    // minimum-distance test is a relaxation problem and cannot be a pure hash),
    // so a species goes on the densest layer whose cellMm is at or below its
    // authored spacing. assetSpeciesTableIsWellFormed refuses the other case.
    uint8_t layer = 0;

    // Per-mille pick weight per biome, indexed by BiomeId. Zero means the
    // species never appears there -- which is also how forge/biomes.py's
    // `hosts` tuple arrives here, since a kind a biome does not host has no
    // weight row to author.
    //
    // THIS IS ALREADY MULTIPLIED BY placement.abundance, at import. The two
    // are separate concepts in the spec (relative share within a biome, versus
    // overall frequency) and their PRODUCT is the only thing a weighted pick
    // can use. Folding them at import rather than here matters: a rare species
    // must be rarely PICKED, so the site goes to a common one instead. Picking
    // first and vetoing on abundance afterwards would leave the site empty and
    // quietly thin out every biome that contains a rare species.
    uint16_t weightPerMille[kBiomeCount] = {};

    // Elevation band, mm above sea level (the world's z = 0 is sea level;
    // kSeaLevelMm). From placement.elev_min_m / elev_max_m.
    int32_t elevMinMm = -10'000;
    int32_t elevMaxMm = 2'000'000;

    // Slope band, in mm of rise per metre of run -- the same currency as
    // kBiomeCliffSlopeMmPerM (biome.h:71), which is 700 for the 70% grade at
    // which a column classifies BARE_ROCK.
    //
    // A BAND, NOT A CEILING, and that is the difference between a boulder and
    // scree. placement.slope_max_pct exists and is authored; there is no
    // slope_min_pct in forge/spec.py yet and it is requested in
    // docs/asset-placement-design.md section 8.3. Until it lands every imported
    // species carries 0 here, which is exactly the old ceiling-only behaviour.
    int32_t slopeMinMmPerM = 0;
    int32_t slopeMaxMmPerM = 450; // placement.slope_max_pct default, 45%

    // Only appears within this distance of a watercourse. 0 means "does not
    // care", which is what 800 of the 828 specs carry.
    //
    // NOT SERVABLE TODAY, and said here rather than discovered. Nothing in
    // voxel-core answers "how far to the nearest water": the samplers answer
    // "is there water at this exact column". AssetColumnFacts::distanceToWaterMm
    // is the input this gate reads and no production caller can fill it yet.
    // See the design doc's section 9.
    int32_t waterMaxMm = 0;

    // Grove strength, Q10. 0 scatters evenly (which, per the header note, is
    // MORE regular than random); kAssetQ10One gathers into stands with open
    // ground between. From placement.cluster.
    uint16_t clusterQ10 = 0;

    // Extent above and below the anchor, mm. Redundant with the layer's own
    // maximum ON PURPOSE: the layer's number is the bound's contract and this
    // one is the individual's truth, and assetSpeciesTableIsWellFormed exists
    // to check that the second never exceeds the first. An asset baked taller
    // than its layer says puts a hole in the world at its own crown, silently
    // (assetplacement.h:118-122).
    int32_t heightMm = 0;
    int32_t depthMm = 0;

    // The lattice this species' baked grid is on, in mm -- AssetGrid::voxelSizeMm().
    // Checked against its layer by assetLayerAdmitsVoxelSize.
    uint32_t voxelSizeMm = uint32_t(kVoxelSizeMm);
};

// What a column says about itself, at the resolution placement needs.
//
// A STRUCT THE CALLER FILLS, rather than a callback onto Amplifier, for the
// reason assetplacement.h gives for templating its bound: "a bound that can only
// be exercised through a full amplifier on real terrain is a bound that will
// only ever be tested in the easy direction". Everything here is available from
// one ColumnSample -- `biome` and `slopeMmPerM` are carried on it as of this
// change, precisely so that this struct can be filled without a second
// evaluation.
struct AssetColumnFacts {
    // THE ONE FIELD THAT IS NOT DATA. False means "I could not evaluate this
    // column" -- an unloaded tile, a declined bound, a query off the edge of
    // anything -- and every entry point below refuses rather than guessing.
    // See THE ANTI-FLOAT GUARD on assetResolveSite.
    bool known = false;

    BiomeId biome = TEMPERATE_FOREST; // ColumnSample::biome
    int32_t surfaceMm = 0;            // ColumnSample::surfaceMm
    int64_t slopeMmPerM = 0;          // ColumnSample::slopeMmPerM

    // IS THE VOXEL topSolidVoxelZ(surfaceMm) POINTS AT ACTUALLY SOLID?
    //
    // THIS IS THE FIELD THAT STOPS TREES STANDING OVER HOLES, and it is not
    // the same question as "is there a surface here". topSolidVoxelZ is a
    // function of surfaceMm alone; the carve passes (caves.h, caverns.h) run
    // AFTER the surface and can turn that exact voxel into air. A cave mouth
    // and a sinkhole shaft are both precisely that -- ground whose stated
    // surface is at 400 m and whose top voxel is the open sky of a shaft.
    //
    // Deriving the anchor from surfaceMm and stopping there is this project's
    // signature failure verbatim: something attached at a point that was
    // derived rather than verified. The verification is one call --
    // Amplifier::materialAt(col, topSolidVoxelZ(col.surfaceMm)) != MAT_AIR --
    // on a ColumnSample the caller already holds, and
    // assetColumnFactsFromSample in assetfield.h is the binding that makes it
    // impossible to forget.
    //
    // Default FALSE, so a caller that fills surfaceMm and forgets this places
    // nothing at all rather than placing everything on faith.
    bool anchorSolid = false;

    // Distance to the nearest watercourse, mm, or kAssetNoWaterDistanceMm for
    // "not known". A species with waterMaxMm == 0 never reads it; a species
    // that does read it is REFUSED when this is unknown, because placing a
    // riverbank willow on the strength of not knowing where the river is is
    // exactly the derived-not-verified failure this file is guarding.
    int32_t distanceToWaterMm = 0;
};

// "I do not know how far the water is." INT32_MAX rather than -1 so that the
// ordinary comparison `distanceToWaterMm <= waterMaxMm` fails closed on its own
// even if a caller forgets the explicit test.
inline constexpr int32_t kAssetNoWaterDistanceMm = 0x7fffffff;

// A site that survived, bound to a species and to a piece of ground.
//
// THE ANCHOR IS IN THE STRUCT AND THERE IS NO WAY TO GET ONE WITHOUT IT. See
// assetResolveSite.
struct AssetInstance {
    int64_t anchorXMm = 0, anchorYMm = 0;
    int32_t anchorZMm = 0;   // the ground surface this instance stands ON
    int64_t anchorVz = 0;    // ... and the VOXEL, by core.h's topSolidVoxelZ --
                             // the same rule surfaceBrickRange materialises
                             // bricks by, verified non-air before this was set
    uint16_t speciesIndex = 0;
    uint16_t bankId = 0;
    uint16_t seedIndex = 0;
    uint8_t layer = 0;
    uint8_t yawQuarter = 0;

    friend bool operator==(const AssetInstance&, const AssetInstance&) = default;
};

// --- the gates --------------------------------------------------------------

// Does this species tolerate this column at all? Integer compares, no hashes --
// this runs once per species per site and is the cheapest thing in the file, so
// it runs first.
inline bool assetSpeciesTolerates(const AssetSpecies& s, const AssetColumnFacts& col) {
    if (!col.known) return false;
    if (uint32_t(col.biome) >= uint32_t(kBiomeCount)) return false;
    if (s.weightPerMille[col.biome] == 0) return false;
    if (col.surfaceMm < s.elevMinMm || col.surfaceMm > s.elevMaxMm) return false;
    if (col.slopeMmPerM < int64_t(s.slopeMinMmPerM)) return false;
    if (col.slopeMmPerM > int64_t(s.slopeMaxMmPerM)) return false;
    if (s.waterMaxMm > 0) {
        // Fails closed on kAssetNoWaterDistanceMm, and deliberately: a species
        // that only grows near water must not be placed by a caller that does
        // not know where the water is.
        if (col.distanceToWaterMm > s.waterMaxMm) return false;
    }
    return true;
}

// The grove field's gain at a position, Q10, mean kAssetQ10One.
//
// Exposed rather than kept private to assetResolveSite because it is the thing
// a measurement has to be able to sample directly -- and because the density-
// preservation property (E[gain] == 1024) is a claim about THIS function that
// a test should be able to check without going through five other gates.
// TWO OCTAVES, MULTIPLIED, and the second one is not decoration -- it is what
// makes the pattern read as stands rather than as gentle unevenness.
//
// A single octave modulates density over [0, 2x] at full strength, and that was
// MEASURED as not enough: index of dispersion 851 (unclustered control) -> 1408,
// i.e. clumped, but only just past Poisson. Two independent mean-1024 fields
// multiplied give [0, 4x] with the mean STILL EXACTLY 1024 -- E[g1*g2] =
// E[g1]E[g2] when the two are independent, and they are, because the seed fold
// below puts them in provably separate fields. The measurement moved to ~1900,
// past twice the control, with the population unchanged.
//
// The two lattices are a FOREST scale and a GROVE scale (12x and 4x the layer's
// own cell pitch), which is the structure a real stand has: broad regions where
// the species is common at all, and clumps inside them.
inline int32_t assetClusterGainQ10(uint64_t seed, const AssetLayer& layer, int32_t layerIndex,
                                   uint16_t speciesIndex, uint16_t clusterQ10, int64_t xMm,
                                   int64_t yMm) {
    if (clusterQ10 == 0) return kAssetQ10One;
    const uint32_t li = static_cast<uint32_t>(layerIndex & (kAssetLayerCount - 1));
    const int64_t grove = int64_t(layer.cellMm) * kAssetClusterLatticeCells;
    const int64_t forest = int64_t(layer.cellMm) * kAssetClusterLatticeCells * 3;
    if (grove <= 0 || forest <= 0) return kAssetQ10One;
    // The species index rides on the SEED, not the channel: the channel block
    // is already spent one per layer, and a per-species seed fold gives an
    // independent field per species at no cost. Without it every species in a
    // biome would clump in exactly the same places. The octave index rides on
    // the same fold, which is what makes the two octaves independent.
    const uint64_t s0 = seed ^ splitmix64(uint64_t(speciesIndex) * 2u + 0x9E37u);
    const uint64_t s1 = seed ^ splitmix64(uint64_t(speciesIndex) * 2u + 0x9E38u);
    // Each f is in [-32768, 32767]. Clamped at zero because a gain cannot be
    // negative; the clamp is unreachable for clusterQ10 <= kAssetQ10One (the
    // authored range) and is here so an out-of-range strength degrades to
    // "never grows in the gaps" rather than to a signed-comparison bug.
    const int64_t f0 = valueNoise2Fade(s0, xMm, yMm, grove, CH_ASSET_CLUSTER + li);
    const int64_t f1 = valueNoise2Fade(s1, xMm, yMm, forest, CH_ASSET_CLUSTER + li);
    int64_t g0 = int64_t(kAssetQ10One) + (int64_t(clusterQ10) * f0) / 32768;
    int64_t g1 = int64_t(kAssetQ10One) + (int64_t(clusterQ10) * f1) / 32768;
    if (g0 < 0) g0 = 0;
    if (g1 < 0) g1 = 0;
    return static_cast<int32_t>(g0 * g1 / int64_t(kAssetQ10One));
}

// THE CLUSTER VETO ITSELF, as one function, because the test and the resolver
// must not be two spellings of it.
//
// `keepPerMille` is the species' UNCLUSTERED chance of taking this site -- the
// joint probability that the site is occupied at all AND that this species won
// the pick. That is the number the gain modulates, and getting it right is the
// whole content of this function.
//
// THE FIRST CUT MODULATED A FIXED 1000 AND LOST 17% OF THE POPULATION. Worth
// writing down, because it looked correct and the arithmetic was mean-
// preserving in isolation:
//
//     keep iff draw * 1024 < 1000 * gain          // draw in [0, 999]
//
// E[gain] is 1024, so E[keep] "should" be 1000 per mille. But keep is CAPPED at
// certainty: once gain exceeds 1024 the test is already true for every draw, so
// the whole upper half of the modulation buys nothing while the lower half
// removes sites at full strength. The result is a one-sided thinning that
// measures as clustering and quietly ships every clustered species at a
// fraction of its authored abundance -- the exact silent-no-op shape this
// project keeps hitting, and the reason
// assetcluster_preserves_density_within_a_few_percent exists. It measured
// -4%, -8% and -17% at the three strengths before this was fixed.
//
// The fix is headroom: modulate the species' own keep probability, which is
// well below certainty in every case except a saturated monoculture, so the
// gain has room to go up as well as down. Where there IS no headroom -- one
// species with a summed weight at or over 1000, occupying every site -- the
// upper half still clips, and that is correct rather than a residual bug: a
// species that already stands on every site of its lattice cannot be clustered
// into denser stands, only thinned out of the gaps.
//
// EXACT INTEGER, NO INTERMEDIATE ROUNDING. The comparison is cross-multiplied
// rather than computed as a per-mille, so a species at 37/491 of the weight is
// not quantised to 75 per mille on the way through.
inline bool assetClusterKeeps(uint64_t seed, const AssetLayer& layer, int32_t layerIndex,
                              uint16_t speciesIndex, uint16_t clusterQ10, int64_t anchorXMm,
                              int64_t anchorYMm, int64_t cellX, int64_t cellY,
                              int64_t keepNumerator, int64_t keepDenominator) {
    if (keepDenominator <= 0 || keepNumerator <= 0) return false;
    const uint32_t li = static_cast<uint32_t>(layerIndex & (kAssetLayerCount - 1));
    // An independent draw from the pick: reusing bits of the pick hash would tie
    // "is this cell inside a grove" to "which species won here".
    const uint64_t h = hash2(seed ^ 0xA5A5A5A5A5A5A5A5ull, cellX, cellY, CH_ASSET_CLUSTER + li);
    const int64_t draw = int64_t((h >> 48) * 1000u >> 16); // [0, 999], as assetSiteInCell
    const int64_t gain = assetClusterGainQ10(seed, layer, layerIndex, speciesIndex, clusterQ10,
                                             anchorXMm, anchorYMm);
    // draw/1000 < (keepNumerator/keepDenominator) * (gain/1024), cross-multiplied.
    // Bounds: draw < 1000, keepDenominator is a sum of per-mille weights over
    // one layer's species list, gain <= 2048. Comfortably inside int64.
    return draw * 1024 * keepDenominator < 1000 * keepNumerator * gain;
}

// --- the resolution ---------------------------------------------------------

// Turn a site into an instance, or refuse.
//
// THE ANTI-FLOAT GUARD IS THIS FUNCTION'S SIGNATURE. `col` is required, it
// carries a `known` flag, and the instance's anchorZMm comes from
// `col.surfaceMm` and from nowhere else. There is no overload that omits the
// column and no way to construct an AssetInstance whose z was derived rather
// than read.
//
// This project's last three silent no-ops were all one shape -- "something
// attached at a point that was derived rather than verified eventually
// detaches" -- and placement's version of it is worse, because it is invisible
// in aggregate: four hundred trees in a chunk, three of them a metre up, and
// nothing anywhere logs it. Refusing to place a tree is a tree that is not
// there; placing one on nothing is a tree in the sky, and only the second reads
// as a rendering bug.
//
// `species` is the WHOLE table; only entries whose `layer` matches the site's
// are considered, so one table serves all four lattices and a species cannot
// leak onto a layer whose bound did not account for it.
inline bool assetResolveSite(uint64_t seed, const AssetLayer* layers, int layerCount,
                             const AssetSpecies* species, int speciesCount, const AssetSite& site,
                             const AssetColumnFacts& col, AssetInstance& out) {
    if (!col.known) return false;
    if (layers == nullptr || species == nullptr || speciesCount <= 0) return false;
    if (site.layer < 0 || site.layer >= layerCount || site.layer >= kAssetLayerCount) return false;
    const AssetLayer& L = layers[site.layer];
    if (L.cellMm <= 0) return false;

    // 1. HABITAT SCAN. No hashes; integer compares only.
    uint32_t total = 0;
    for (int i = 0; i < speciesCount; ++i) {
        if (int(species[i].layer) != site.layer) continue;
        if (!assetSpeciesTolerates(species[i], col)) continue;
        total += species[i].weightPerMille[col.biome];
    }
    if (total == 0) return false;

    // 2. PICK, weighted by biome weight x abundance.
    //
    // THE PICK RUNS BEFORE THE OCCUPANCY TEST, and that ordering is not
    // cosmetic. Occupancy and clustering are ONE test (see step 3) because the
    // cluster gain has to modulate a probability with headroom above it, and
    // the only such probability is this species' own joint chance of taking the
    // site. Testing occupancy first and clustering afterwards is what lost 17%
    // of the population -- assetClusterKeeps records the measurement.
    const uint64_t h = hash2(seed, site.cellX, site.cellY, CH_ASSET_SPECIES + uint32_t(site.layer));
    const uint32_t cap = total > 1000u ? 1000u : total;
    uint32_t r = static_cast<uint32_t>((h >> 8) % uint64_t(total));
    int chosen = -1;
    for (int i = 0; i < speciesCount; ++i) {
        if (int(species[i].layer) != site.layer) continue;
        if (!assetSpeciesTolerates(species[i], col)) continue;
        const uint32_t w = species[i].weightPerMille[col.biome];
        if (r < w) { chosen = i; break; }
        r -= w;
    }
    // Unreachable: the walk sums exactly `total` and r < total. Refuse rather
    // than assert, because the alternative to refusing is emitting species 0.
    if (chosen < 0) return false;

    // 3. OCCUPANCY AND CLUSTERING, as ONE test.
    //
    // `cap` -- the summed weight, capped at certainty -- is the chance the site
    // is used AT ALL. The pick above has already distributed among species in
    // proportion to their weights, so the joint chance that this species takes
    // this site is (weight/total) x (cap/1000), and the test here supplies only
    // the second factor. Multiplying the weight in again here would square it:
    // the first cut did exactly that and dropped a saturated rainforest from
    // every site to one in three, which assetpolicy_summed_weight_thins_a_poor_
    // biome_without_a_density_table caught by asserting the RICH end as well as
    // the poor one.
    //
    // `cap` is also what makes a desert sparse without a per-biome density
    // table anywhere: where only one low-weight species is eligible, most sites
    // carry nothing.
    //
    // WHERE cap IS 1000 THERE IS NO HEADROOM AND CLUSTERING DOES NOTHING, and
    // that is correct rather than a residual defect: a set of species whose
    // weights already saturate the lattice stands on every site of it, and a
    // veto cannot gather into denser stands what is already everywhere. It can
    // only thin, which would change the population. Species-level groves inside
    // a saturated biome -- beech here, oak there, at unchanged total cover --
    // need the cluster field folded into the PICK weights instead, which costs
    // one field evaluation per eligible species rather than one per site. Not
    // done; see the design doc's section 9.
    const AssetSpecies& S = species[chosen];
    if (!assetClusterKeeps(seed, L, site.layer, uint16_t(chosen), S.clusterQ10, site.anchorXMm,
                           site.anchorYMm, site.cellX, site.cellY, int64_t(cap), 1000))
        return false;

    // 5. GROUND, VERIFIED. `anchorSolid` is the caller's answer to
    // "materialAt(col, topSolidVoxelZ(surfaceMm)) != MAT_AIR", and a false
    // here is a cave mouth or a sinkhole shaft: ground whose stated surface is
    // real and whose top voxel is open air. Refusing is a tree that is not
    // there; not refusing is a tree hanging over a hole, and only the second
    // one reads as a rendering bug.
    if (!col.anchorSolid) return false;
    out.anchorXMm = site.anchorXMm;
    out.anchorYMm = site.anchorYMm;
    out.anchorZMm = col.surfaceMm;
    out.anchorVz = topSolidVoxelZ(col.surfaceMm);
    out.speciesIndex = static_cast<uint16_t>(chosen);
    out.bankId = S.bankId;
    out.seedIndex = site.seedIndex;
    out.layer = static_cast<uint8_t>(site.layer);
    out.yawQuarter = site.yawQuarter;
    return true;
}

// --- load-time checks -------------------------------------------------------

// Why a species table was refused. Named rather than a bool for the reason
// assetgrid.h gives: "the only thing worse than refusing to load an asset is
// refusing to say which asset and why".
enum class AssetTableError : uint8_t {
    kOk = 0,
    kBadLayer,       // filed on a layer that does not exist
    kTooTall,        // taller than its layer's maxHeightMm -- a hole at its own crown
    kTooDeep,        // reaches below its layer's maxDepthMm
    kWrongLattice,   // 10 cm asset on a detail layer, or a 5 cm one on a terrain layer
    kSpacingTooTight,// authored spacing finer than its layer's cell can express
    kNoBiome,        // every biome weight is zero: it can never appear anywhere
};

inline const char* assetTableErrorText(AssetTableError e) {
    switch (e) {
        case AssetTableError::kOk: return "ok";
        case AssetTableError::kBadLayer: return "species filed on a layer that does not exist";
        case AssetTableError::kTooTall:
            return "species is taller than its layer's maxHeightMm: the streaming bound would "
                   "prove its crown to be air, which is a hole in the world";
        case AssetTableError::kTooDeep: return "species reaches below its layer's maxDepthMm";
        case AssetTableError::kWrongLattice:
            return "species voxel size does not match its layer's lattice (terrain layers are "
                   "kVoxelSizeMm and nothing else; detail layers are anything but)";
        case AssetTableError::kSpacingTooTight:
            return "species spacing is finer than its layer's cell pitch can express";
        case AssetTableError::kNoBiome: return "species has a zero weight in every biome";
    }
    return "unknown";
}

// Check one species against its layer. This is the check that turns "this tree
// is taller than its layer says" into a refusal by the tool that files it,
// rather than into a player noticing that tall trees have flat tops -- which is
// what the failure looks like, and which reads as a generator bug rather than a
// bounds bug.
//
// `spacingMm` is the species' authored placement.spacing_m; pass 0 to skip that
// one check (an importer that has not read it yet).
inline AssetTableError assetSpeciesFits(const AssetSpecies& s, const AssetLayer* layers,
                                        int layerCount, int32_t spacingMm) {
    if (layers == nullptr || int(s.layer) >= layerCount || int(s.layer) >= kAssetLayerCount)
        return AssetTableError::kBadLayer;
    const AssetLayer& L = layers[s.layer];
    if (s.heightMm > L.maxHeightMm) return AssetTableError::kTooTall;
    if (s.depthMm > L.maxDepthMm) return AssetTableError::kTooDeep;
    if (!assetLayerAdmitsVoxelSize(L, s.voxelSizeMm)) return AssetTableError::kWrongLattice;
    if (spacingMm > 0 && spacingMm < L.cellMm) return AssetTableError::kSpacingTooTight;
    bool any = false;
    for (int b = 0; b < kBiomeCount; ++b)
        if (s.weightPerMille[b] != 0) any = true;
    if (!any) return AssetTableError::kNoBiome;
    return AssetTableError::kOk;
}

} // namespace vxc
