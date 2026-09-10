# Temperate understory expansion

Scope: 113 existing temperate-forest-allowed profiles: 20 bushes, 10 reeds,
38 grass-category profiles, and 45 flowers. At 36 seeds this is 4,068 outputs.
The grass category includes ferns, mosses and submerged plants; wetland and
water-edge plants belong to biome microhabitats rather than dry forest floor.
The two jungle-labelled archetypes are retained as explicit temperate replacements:
Great wood-rush (Luzula sylvatica) and False Solomon's seal (Maianthemum racemosum).
Their legacy IDs remain stable. These are selected botanical reference analogues,
not claims that the old generic geometry depicted those taxa.

All work starts at 25 mm cubic voxels and spring appearance. Photo collection,
manual comparison, generator acceptance and user endorsement are separate gates.
No old asset is removed before its replacement passes the visual and artifact gates.
Existing endorsements and rejections remain authoritative. No world bank publication.

Sequence:
1. Inventory exact scope and snapshot existing generator identity.
2. Isolate recipe dependencies so unchanged tree approvals remain valid.
3. Assemble attributed remotely hosted botanical photos, resolving ambiguous taxa.
4. Refactor by actual plant architecture: woody stools, canes, blades, fronds,
   rosettes, runners, moss mats, floating leaves, submerged whorls and flower stems.
5. Inspect small, medium and large 25 mm pilots against source photographs.
6. Stage 36 deterministic variants, inspect each contact sheet and validate bytes.
7. Accept source seed 7, install pending variants, retire superseded unendorsed
   assets with provenance, and verify persistent Forge decisions and exact export.

Current state: scope inventoried; implementation and reference review in progress.
This document is not an acceptance record.

## Completed collection, 8 September 2026

All 113 sources passed photographic comparison, three current size pilots and
their full 36-variant sheets. All 4,068 variants are installed pending choices.
Authoritative records reside in `library/<species>/species.json`; user
endorsement remains separate. Twelve superseded assets were retired after
replacement acceptance. No scoped old active assets remain. Cattail's initial
nodal reed leaves were replaced with basal strap leaves before acceptance.

Physical scale is retained: small woodland flowers and wood-sorrel leaflets
are only a few 25 mm cells across. Petal counts may exist in the generator
without remaining individually visible after rasterization. Reviews explicitly
record this limit; plants and flowers are not enlarged to hide it.

The dependency fingerprint now isolates the selected architecture, its reachable
helpers, and its species parameters. Unrelated understory edits preserve all
49 accepted tree identities. When an identity implementation change affects an
already accepted source, every saved VXA is rebuilt and compared byte-for-byte
before updating provenance; user decision flags and artifact bytes stay intact.

The review gallery is `/static/understory-review/index.html`. Exact comparison
notes are in `out/understory-review/manual-findings.json`; artifact-bound reviews
are also copied into the species library. Signed botanical image URLs can expire;
`tools/understory_references.py <species>` refreshes those references without
discarding other providers or manual findings. Remote images are never downloaded.

## Placeholder scope resolution

The original temperate design explicitly lists `jungle-groundcover` as a generic
broad-leaved shade-floor plant and `jungle-understory-flower` as a generic tall
shade bloom (`docs/biomes/03-temperate-forest.md`, lines 130 and 161). Therefore
their biome assignment was intentional; silently excluding them based only on
their IDs would discard a documented part of the forest plan.

The replacement references are [RHS great wood-rush](https://www.rhs.org.uk/plants/10579/luzula-sylvatica/details)
and [Missouri Botanical Garden false Solomon's seal](https://plantfinder.mobot.org/PlantFinderDetails.aspx?taxonid=282418).
Both suit woodland shade, the latter with spring terminal white flower panicles.
The application uses clear display names while preserving the legacy identifiers
and any historical decisions. All 113 profiles are consequently included.

## Completion status

All 113 profiles and 4,068 variants are complete. The final report supersedes intermediate checkpoints below or in historical logs. See [completion report](understory-completion-report.md) for authoritative final counts, validation and limitations.
