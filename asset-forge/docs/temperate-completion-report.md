# Temperate forest collection completion — 2026-09-07

49 reference-reviewed profiles; 36 distinct seeded variants each; 1,764 saved
outputs. Every production output uses 100 mm cubic voxels (zero at 50 or 25 mm).
This is a spring stylized vertical-slice collection, not a botanical simulation.
Profiles include regional species and habitat archetypes; they are not 49 taxa
that necessarily coexist in one natural forest.

## Source authority and review

`library/<species>/species.json` records the baseline, current generator digest,
reference seed 7, its exact variant identity, and all 36 variant hashes.
`reference-review.json` records photo sources and image identities, manual visual
findings, reviewed small/medium/large seeds 1/4/7, and the exact 36-image sheet hash.
The [reference gallery](http://127.0.0.1:8731/static/tree-reference-review.html)
contains all 49 profiles and the reviewed silhouette, branch-only and shaded views.
Other-season photographs inform structure only; foliage appearance remains spring.

The generator uses attached shoot foliage, hierarchical forks, dominant leaders,
pendulous shoots, irregular conifer boughs, mature pine crowns and fern rachises.
The canonical temperate oak retains its reviewed oak architecture. Species tuning
controls crown proportions, branching, bark, density and habitat form.

## Forge, endorsement and game use

[Open Forge](http://127.0.0.1:8731/?tab=forge&species=temperate-oak).
Choose a source, inspect its reference and seed variants, regenerate or extend a
seed range, then endorse desired outputs. The embedded 3D viewer supports orbit,
zoom and a toggleable 1.8 m player pawn. Production is now part of Forge; the old
standalone Production tab and old Forge controls have been removed.

All 1,764 variants remain pending your endorsement. Source acceptance records the
design review; it does not impersonate your selection of assets for the world.
Pending bytes are in `out/forge-candidates/<species>/`, with their authoritative
inventory in the species library record. Endorsement moves exact saved bytes into
`library/<species>/<variant>/`. Asset Library and game export exclude candidates.
Game export copies the endorsed VXA rather than running the generator again.
No global game bank/manifest publication or world placement-weight change was made.

Each collection has 12 small, 12 medium and 12 large variants, and 12 each of
open-grown, woodland and edge forms. Every size/form combination has four seeds.
Seeds are deterministic numeric inputs; variants are the resulting saved assets.

## Verification and cost

- All 1,764 outputs passed artifact checksum, distinct-geometry, shared-baseline,
  current generator, resolution, health-report and source-reference checks.
- All 147 small/medium/large pilots and all 49 batch sheets were visually reviewed.
- Full local batch time summed across species: 2,584.12 seconds (43.1 minutes),
  using eight workers. Seed generation made zero paid model/API calls; this is
  separate from the cost of Astra's review and code work.
- Tundra pine regeneration reused all 36 verified cached outputs in 1.06 seconds.
- Deterministic oak rebuild, exact endorsement, cache-corruption rejection and
  failed-run reporting probes passed. 799 unrelated spec hashes were unchanged
  by the inventory probe.
- Exact game-bank export passed for beech and oversized sequoia fixture geometry;
  measured query bounds agree across bank/manifest tools without changing legacy
  filing. Sequoia required a temporary dense placement fixture because its real
  landmark spacing rounds to zero scatter weight. Landmark placement remains a
  separate world-design task.
- TypeScript checking and Vite production build passed. Live API verification
  found all 49 current accepted sources, all 1,764 variants, no retired oak aliases,
  and no pending output exposed as an endorsed library asset.

77 superseded stored assets were removed, including obsolete oak family/pilot
aliases. Per-species retirement ledgers preserve previous IDs and specs without
keeping selectable old geometry. The retired pilot installer cannot recreate its
obsolete source. Existing unrelated assets and historical research artifacts were
left alone.

## Remaining art limits

The shared voxel palette does not model leaf undersides or species-specific white
flower bracts. Dogwood, hawthorn, pear and crab apple represent leaf-on spring,
not peak white bloom. Pine crowns can be fuller than exposed windswept specimens.
Tree fern pinnae are simplified at 100 mm, and some birch/alder leaders have small
bare tip cells. These limits are recorded for future refinement. User endorsement
is the next art-direction checkpoint before world publication.

## Collection inventory

Every row has 36 variants, reference seed 7, 100 mm voxels and pending endorsement.

| Profile | Height range (m) | Batch seconds |
|---|---:|---:|
| american-beech | 7.2–26.2 | 47.7 |
| european-beech | 8.3–31.5 | 65.0 |
| sugar-maple | 7.2–26.5 | 67.5 |
| bigleaf-maple | 7.3–26.5 | 83.5 |
| sycamore-maple | 7.1–26.4 | 85.6 |
| field-maple | 3.2–10.9 | 13.3 |
| japanese-maple | 1.8–5.9 | 3.9 |
| hornbeam | 5.8–21.3 | 28.5 |
| small-leaved-lime | 7.0–26.5 | 40.2 |
| common-ash | 5.8–21.1 | 44.2 |
| field-elm | 3.4–11.7 | 9.4 |
| wych-elm | 5.9–21.1 | 27.7 |
| birch | 4.7–17.5 | 13.2 |
| common-alder | 5.3–19.6 | 23.4 |
| eastern-cottonwood | 7.1–26.3 | 37.8 |
| quaking-aspen | 5.4–19.6 | 15.8 |
| white-poplar | 5.9–21.2 | 22.6 |
| weeping-willow | 3.1–12.1 | 20.3 |
| black-cherry | 5.7–21.4 | 20.9 |
| cherry-blossom | 2.2–7.2 | 4.8 |
| flowering-dogwood | 2.7–9.1 | 6.7 |
| hawthorn-scrub | 1.4–4.3 | 3.2 |
| crab-apple | 2.0–6.6 | 4.9 |
| wild-pear | 2.6–8.6 | 6.2 |
| rowan | 2.6–8.6 | 10.8 |
| shagbark-hickory | 7.3–26.2 | 75.5 |
| sweet-chestnut | 7.3–26.7 | 42.6 |
| tulip-tree | 10.2–38.1 | 46.9 |
| honey-locust | 5.2–19.0 | 33.9 |
| bur-oak | 4.2–16.0 | 61.6 |
| cork-oak | 3.2–12.7 | 55.1 |
| holm-oak | 3.3–12.7 | 53.7 |
| temperate-oak | 3.9–14.4 | 57.2 |
| river-broadleaf | 3.7–13.1 | 11.7 |
| temperate-sapling | 1.6–5.0 | 5.1 |
| douglas-fir | 11.8–43.6 | 125.7 |
| eastern-hemlock | 8.9–32.7 | 115.8 |
| western-hemlock | 10.4–38.1 | 127.3 |
| western-red-cedar | 13.3–49.0 | 189.0 |
| norway-spruce | 10.4–38.1 | 108.5 |
| sitka-spruce | 13.2–49.0 | 143.8 |
| scots-pine | 7.5–26.5 | 86.4 |
| maritime-pine | 6.0–21.2 | 66.2 |
| tundra-pine | 2.9–9.7 | 22.7 |
| monterey-cypress | 3.7–12.8 | 26.9 |
| columnar-cypress | 4.0–14.3 | 78.3 |
| european-yew | 3.6–13.0 | 27.3 |
| hero-sequoia | 23.4–87.1 | 301.3 |
| tree-fern | 2.2–7.9 | 14.4 |
