# Temperate forest ecological placement proposal

Date: 2026-09-09
Status: Scope confirmed by user on 2026-09-09; implementation and runtime validation in progress. This document defines the agreed scope; current evidence and remaining work are recorded in [the placement status](temperate-ecological-placement-status.md). Completion has not been established.

## Intent

Generate recognizable plant communities and forest structure, using the temperate slice as the test bed for a reusable environment placement system. Seeded chance remains useful, but species, size, spacing and ground cover must be conditional on shared habitat and neighboring vegetation. Spring appearance remains fixed. Understory remains instanced, non-colliding and not individually voxel-destructible.

## Foundation and gaps at the initial audit

The initial audit inspected voxel-core/include/voxelcore/assetpolicy.h, assetplacement.h, assetfield.h, docs/asset-placement-architecture.md and placement-spec-schema.md. At that point policy provided deterministic sites, biome/species weights, slope and water gates, per-kind density and a two-octave grove field. The resolver transferred site.seedIndex directly into the output without selecting against local age/crown requirements. Spacing was tied to candidate lattices/weighting. The subsequent ecological implementation adds role-aware selection and bounded competition; the original gaps here are historical context, not a description of the current resolver.

Preserve the streaming bound contract: a policy may reject a candidate but may not move it or choose geometry outside its layer's declared bounds. A denser candidate envelope or larger specimen requires an explicit layer/bounds revision and validation, not an ecological-policy workaround.

Older design documents contain historical asset counts/resolutions. They are architectural context, not authoritative current inventory.

## Research

Minecraft Bedrock documents feature passes, biome filtering, weighted choices among different tree sizes, and scatter distributions. These are useful compositional primitives, not evidence of a full competition/succession simulation. Do not assume Bedrock implementation details apply verbatim to Java.

- https://learn.microsoft.com/en-us/minecraft/creator/documents/world-generation?view=minecraft-bedrock-stable
- https://learn.microsoft.com/en-us/minecraft/creator/reference/content/featuresreference/examples/features/minecraft_scatter_feature?view=minecraft-bedrock-stable

Vintage Story's public vegetation source reads separate forest, shrub and climate maps and has ordered shrub/tree/patch passes. Its forest-floor system ties floor treatment and patches to generated trees. This is a useful precedent for dependent ground cover rather than unrelated scatter. Public repository master is a moving implementation reference, not a pinned release guarantee.

- https://github.com/anegostudios/vsessentialsmod/blob/master/Systems/WorldGen/Standard/ChunkGen/8.GenVegetationAndPatches/GenVegetationAndPatches.cs
- https://github.com/anegostudios/vsessentialsmod/blob/master/Systems/WorldGen/Standard/ChunkGen/8.GenVegetationAndPatches/Treegen/ForestFloorSystem.cs

Newlands and Zauner demonstrate a forest-generation system using ecosystem simulation. This establishes a further design option; it does not establish that we need to run a living ecosystem during gameplay.

- https://arxiv.org/abs/2208.01471

## Proposed ordered pipeline

1. Habitat: sample existing climate, elevation, slope, exposure and hydrology. Audit availability and semantics of each input before adding soil fertility, drainage or wetness proxies. Missing channels must be explicit, not silently invented as ground truth.
2. Communities: regional mixtures of compatible species with a few local dominants and less common companions. All 49 profiles need eligibility/role records, not equal representation in every stand.
3. Stand structure: spatially coherent maturity, productivity, density and disturbance history. Initial tuning bands could span tens to hundreds of metres; these are artistic starting values, not measured ecological constants. Build young cohorts, mature mixed stands, gap regeneration and occasional old specimens.
4. Tree candidates: choose a species and target structural class from habitat plus stand context. Select only endorsed, published variants whose measured height/crown/base geometry fits that role. Asset seeds reproduce shapes; they are not size or age labels. Do not scale 100 mm tree voxels arbitrarily. Record gaps where the approved collection lacks a needed form.
5. Competition: deterministic, bounded neighbor comparisons. Separate trunk/root exclusion from softer crown overlap; allow overlapping canopies where appropriate. Use a fixed number of priority rounds or bounded local thinning, not recursively dependent acceptance chains. Regional boundary halos must include the total influence radius of every pass. Ownership and hash keys must be independent of chunk generation order and catalog row order.
6. Canopy and floor: derive a cheap cover/light proxy from accepted tree crowns, including openness and edges. Use that plus moisture and substrate to choose understory communities. Shade-tolerant vegetation beneath appropriate stands; light-demanding grasses, shrubs and flowers in suitable openings; wetland plants in verified water/depth bands. Spring-flowering woodland herbs need explicit exceptions to a simplistic shade exclusion rule.
7. Other environment assets: rocks respond to substrate, exposure and slope processes rather than tree-density rules. Moss and similar cover can respond to dampness and nearby supports. Fallen timber/stumps require approved source assets and placement envelopes; do not assume the library already contains them.
8. Uncommon features: bounded event patches for ancient groves, unusual specimens, windthrow gaps, flower openings or resource-rich patches. Increase frequency within suitable habitat, not everywhere. Give each event a footprint, spacing, budget and compatible follow-on vegetation. Tune rates in encounters per travel distance/time as well as events per square kilometre. Keep rare-resource yield independently controlled.

## Ownership and reproducibility

Asset Library remains authoritative for species metadata, approved variant IDs, measured geometry, appearance and ecological traits. Separate versioned placement/community rules reference these IDs and are compiled into a pinned world-generation manifest. Forge may expose placement suitability and a neighborhood preview; it should not become a second inventory.

Distinguish world seed, stable placement/stand ID and asset variant seed. Pin placement-rule version and publication manifest identity for reproducible tests. The user explicitly permits overwriting existing development saves and player edits: migration compatibility and preservation are not requirements for this phase. Regenerate development test worlds as needed. Avoid deleting unrelated files or saves merely to update placement rules. Long-term save migration is deferred.

## Delivery stages

A. Inventory and data audit: map all temperate environment profiles to ecological roles, approved geometry and available habitat channels. Produce a missing-data/asset report.
B. Headless placement prototype: compare the old scatter against community, cohort and canopy-aware placement on identical terrain. Export overhead maps, per-instance decisions and measurements before changing the game renderer.
C. Representative test region: dry ridge, moist hollow, closed stand, gap, edge and shoreline. Show player-scale views and movement routes. Compare equal-density controls to separate structural improvements from simply adding more assets.
D. Integrate shared placement decisions into terrain trees/rocks and instanced understory. Preserve existing rendering and appearance paths. Add clear debug explanations for why an asset was selected or rejected.
E. Tune and accept: evaluate multiple world seeds, then expand the same system beyond the temperate slice.

## Acceptance criteria

Identical outputs across chunk order, worker count and reload; no boundary seams or duplicate ownership; stable variant identities; no unsupported, submerged or overlapping trunks outside explicit exceptions; conservative streaming bounds; realistic-looking local species dominance and size relationships; ground-cover response to canopy and moisture; event budgets respected; all runtime assets endorsed and published.

Measure stems/hectare by species and structural class, nearest-neighbor distances, crown overlap, canopy cover, gap sizes, understory cover by habitat, visual access and usable openings. Measure generation CPU time, peak memory, instance/draw counts, streaming stalls and GPU/frame time under representative density. Thresholds need pilot measurements and user gameplay priorities, not invented performance promises.

## Confirmed scope and implementation direction

User decisions on 2026-09-09:

- Initial world generation only. No ongoing growth, succession, regrowth, or full ecological simulation.
- Mixed natural forest with regional communities. Each locality has a limited set of dominant and companion species; the full library is distributed across suitable regions.
- Ancient groves, large trees and dense thickets should occur more frequently than a strict realism baseline. These are the first three authored feature types. Other feature proposals are optional later additions, not equal-priority commitments.
- Aim for the readable exploration and building availability the user associates with Minecraft and Vintage Story, while retaining natural patterns. This is an experience target, not a claim that either game guarantees paths or building plots.
- Build a reusable placement system for other biomes and the whole world. Temperate forest is its first configured test case.
- Existing development saves and edits may be overwritten. No compatibility/migration project is needed now.

Use authored stand classes to suggest history without simulating it: young patches, mixed mature stands, open old groves and disturbance gaps. A sampled maturity/structure field chooses size distributions directly. Bounded local spacing checks suggest competition without advancing virtual years.

Readability should emerge from contrasting vegetation structures: mature stands with comparatively open ground, sunny clearings and edges, and localized thickets with walk-around space. Do not carpet the entire forest in eye-height bushes. Understory currently has no collision; thicket density therefore affects visibility, not physical movement blocking. Test trunk clearance and sightlines separately. Bias openings toward naturally suitable low-slope ground; do not flatten terrain or carve artificial corridors as part of vegetation placement. Building availability means finding usable ground, not guaranteeing a prepared building pad at fixed intervals.

For the three promoted feature types, author separate frequency, footprint, minimum-separation and habitat constraints. Ancient groves need a coherent grouping of mature/large forms and suitable floor treatment. Large specimens can also occur outside groves at controlled rates. Dense thickets remain concentrated patches, with spacing that preserves alternating readable areas. Tune their abundance through player-scale walkthroughs and map statistics; no unsupported numerical frequency is fixed yet.

Implement a biome-independent core with data-defined habitat responses, community mixtures, stand classes, variant roles and feature definitions. Compile temperate settings into that shared core. Keep deterministic world-coordinate keys, bounded neighbor queries and explicit influence radii so chunk boundaries and generation order cannot alter results. Keep tree/rock world voxels and instanced understory as separate output consumers of the same placement decisions.

First prototype milestone: deterministic regional community and stand maps, role-aware selection of published variants, tree spacing, canopy-aware understory and the three promoted features, demonstrated on representative temperate terrain. Deliver comparative maps, player-scale views, selection explanations and generation/rendering measurements. Then integrate and tune the shared implementation across multiple world seeds. No additional scope confirmation is required to pursue this agreed direction.


### Authoritative water-mask export for neighbor reconciliation

New bakes retain `BakeResult.placement_water_mask_packed`: fine-resolution lake, final river, and sea coverage from actual B7 surface samples, packed into little-bit-order rows. This preserves the existing B7 wet-set definition before elevation is converted into spline control points. It adds approximately 8 MiB per production tile to the result, without altering current runtime tile bytes.

With `pregen --bake-npz-dir`, successful encodes also write `<x>_<y>.water-source.npz`. Metadata binds the source to the exact encoded tile SHA-256, seed, coordinates, provider identity, cell pitch, and schema. Files are replaced atomically. `read_bake_water_source` refuses a source whose encoded-tile binding differs. Cached tiles that are skipped do not acquire these artifacts retroactively; they must be rebaked in an isolated namespace to obtain authoritative complete masks. `read_published_freshwater_mask` is a diagnostic fallback for existing lake/river rasters and explicitly does not include sea.

`distance_from_final_masks` requires all nine final masks and a halo covering the full representable distance. The publication pass verifies every source binding and common provider/seed/pitch, calculates distances, and publishes under a new bake identity. The isolated nine-tile rebake and center-tile publication are complete; `world-capture-15-forest-shore` validated the corrected center at a fully temperate shoreline. All 1,024 runtime terrain samples matched preflight elevation, slope, biome and water distance, and all 121 exported water-reed anchors were within 8 m of water. This is local placement evidence, not whole-world hydrology or visual/playability acceptance. The original game cache remains unchanged, the eight neighbor distance fields are not reconciled, and these exports do not repair inconsistencies in the underlying lake geometry itself.

The staging command is now implemented (not activated):

```powershell
python -m terrain_service.bake.hydrology_publish --tiles <source-s16> --masks <bake-npz-dir> --output <fresh-output> --target -11 -6
```

Repeat `--target X Y` for additional tiles. Every requested target needs all nine hash-bound source masks. The command verifies shared provider/seed/pitch, pins tile and mask hashes, keeps memory bounded to one target neighborhood, replaces only the decoded placement-distance channel, and writes a content-derived publication ID. It writes the manifest last, so a failed partial run lacks a completion marker. This is staging only: game cache activation and real-world habitat acceptance are separate requirements. Existing terrain without complete bake-mask artifacts must first be rebaked in isolation.
