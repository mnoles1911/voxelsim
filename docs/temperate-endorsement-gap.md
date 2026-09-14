# Temperate forest: species awaiting an endorsement decision

Generated 2026-09-10 from `asset-forge/out/ecological-placement/previews/full-forest-low-cover-1/placement.json` against the game library.

**Endorsed** means all three of: `review_status` is `endorsed`, `visual_approved` is true, and the variant is
not an inventory candidate. That is the exact gate `publish_temperate_appearance.py` applies, so anything short
of it cannot reach the game library no matter how good it looks.

## The gap

| | |
|---|---|
| Species the slice places | 162 |
| Species with an endorsed variant | 2 (birch, temperate-oak) |
| **Species awaiting a decision** | **160** |
| Variants in the private fixture awaiting a verdict | 480 |
| Realized variant folders for them in the library | 0 |
| Variants their `species.json` files declare | 5,760 |

Every endorsed variant today is a tree (birch and temperate oak). **No ground cover of any kind is endorsed.**
The size culling, authored distant shapes, mesh retirement and prewarm work all concern ground cover, so none of
it is exercised by what the shipping library can currently build.

## One thing to know before planning the review

These 160 species have `ecology.json`, `species.json` and `reference-review.json` in the library, and all 162 have
an approved generator and a passed reference review. What they do **not** have is realized variant folders: no
`meta.json`, `spec.json` or `tree.vxa` under `asset-forge/library/<species>/<species>-NNNN/`. Compare
`asset-forge/library/birch/` (nine variant folders) with `asset-forge/library/bracken/` (none).

The variants that exist live only in the private fixture as baked banks. So the work is two steps, not one:
realize the chosen variants into the library from their approved generators and seeds, then review and endorse.
The second step is yours; the first is mechanical.

## The list

Grouped by kind, then alphabetical. `max h` is the tallest variant in the fixture. Pitch is the voxel size the
asset is baked at: trees on the 100 mm world lattice, everything else on 25 mm.

### tree — 47 species, 141 variants

| species | variants | pitch | max h | cover role |
|---|---|---|---|---|
| american-beech | 3 | 100 mm | 22.6 m | - |
| bigleaf-maple | 3 | 100 mm | 22.6 m | - |
| black-cherry | 3 | 100 mm | 17.9 m | - |
| bur-oak | 3 | 100 mm | 13.7 m | - |
| cherry-blossom | 3 | 100 mm | 6.1 m | - |
| columnar-cypress | 3 | 100 mm | 12.2 m | - |
| common-alder | 3 | 100 mm | 16.7 m | - |
| common-ash | 3 | 100 mm | 18.2 m | - |
| cork-oak | 3 | 100 mm | 10.8 m | - |
| crab-apple | 3 | 100 mm | 5.6 m | - |
| douglas-fir | 3 | 100 mm | 37.1 m | - |
| eastern-cottonwood | 3 | 100 mm | 22.5 m | - |
| eastern-hemlock | 3 | 100 mm | 27.9 m | - |
| european-beech | 3 | 100 mm | 26.8 m | - |
| european-yew | 3 | 100 mm | 11.0 m | - |
| field-elm | 3 | 100 mm | 10.2 m | - |
| field-maple | 3 | 100 mm | 9.2 m | - |
| flowering-dogwood | 3 | 100 mm | 7.8 m | - |
| hawthorn-scrub | 3 | 100 mm | 3.8 m | - |
| hero-sequoia | 3 | 100 mm | 74.1 m | - |
| holm-oak | 3 | 100 mm | 10.8 m | - |
| honey-locust | 3 | 100 mm | 16.2 m | - |
| hornbeam | 3 | 100 mm | 18.2 m | - |
| japanese-maple | 3 | 100 mm | 5.2 m | - |
| maritime-pine | 3 | 100 mm | 18.2 m | - |
| monterey-cypress | 3 | 100 mm | 11.1 m | - |
| norway-spruce | 3 | 100 mm | 32.5 m | - |
| quaking-aspen | 3 | 100 mm | 16.7 m | - |
| river-broadleaf | 3 | 100 mm | 11.2 m | - |
| rowan | 3 | 100 mm | 7.7 m | - |
| scots-pine | 3 | 100 mm | 22.6 m | - |
| shagbark-hickory | 3 | 100 mm | 22.6 m | - |
| sitka-spruce | 3 | 100 mm | 41.7 m | - |
| small-leaved-lime | 3 | 100 mm | 22.5 m | - |
| sugar-maple | 3 | 100 mm | 22.7 m | - |
| sweet-chestnut | 3 | 100 mm | 22.4 m | - |
| sycamore-maple | 3 | 100 mm | 22.6 m | - |
| temperate-sapling | 3 | 100 mm | 4.3 m | - |
| tree-fern | 3 | 100 mm | 6.7 m | - |
| tulip-tree | 3 | 100 mm | 32.4 m | - |
| tundra-pine | 3 | 100 mm | 8.4 m | - |
| weeping-willow | 3 | 100 mm | 10.2 m | - |
| western-hemlock | 3 | 100 mm | 32.5 m | - |
| western-red-cedar | 3 | 100 mm | 41.7 m | - |
| white-poplar | 3 | 100 mm | 18.0 m | - |
| wild-pear | 3 | 100 mm | 7.6 m | - |
| wych-elm | 3 | 100 mm | 17.9 m | - |

### bush — 20 species, 60 variants

| species | variants | pitch | max h | cover role |
|---|---|---|---|---|
| blackthorn-scrub | 3 | 25 mm | 3.2 m | shrub |
| box | 3 | 25 mm | 2.33 m | shade-shrub |
| bramble-thicket | 3 | 25 mm | 1.57 m | shrub |
| butchers-broom | 3 | 25 mm | 0.93 m | shade-shrub |
| common-broom | 3 | 25 mm | 2.1 m | shrub |
| common-dogwood | 3 | 25 mm | 3.08 m | shrub |
| dog-rose | 3 | 25 mm | 2.7 m | shrub |
| elder | 3 | 25 mm | 3.62 m | shrub |
| gorse | 3 | 25 mm | 2.08 m | shrub |
| guelder-rose | 3 | 25 mm | 3.08 m | shrub |
| hazel-coppice | 3 | 25 mm | 3.55 m | shade-shrub |
| holly-understorey | 3 | 25 mm | 3.4 m | shade-shrub |
| mountain-laurel | 3 | 25 mm | 3.52 m | shade-shrub |
| red-huckleberry | 3 | 25 mm | 2.27 m | shade-shrub |
| rhododendron-thicket | 3 | 25 mm | 3.48 m | shade-shrub |
| salal | 3 | 25 mm | 1.27 m | shade-shrub |
| snowberry | 3 | 25 mm | 1.88 m | shrub |
| spindle | 3 | 25 mm | 3.2 m | shrub |
| wild-privet | 3 | 25 mm | 2.77 m | shrub |
| witch-hazel | 3 | 25 mm | 3.5 m | shade-shrub |

### reed — 10 species, 30 variants

| species | variants | pitch | max h | cover role |
|---|---|---|---|---|
| branched-bur-reed | 3 | 25 mm | 0.78 m | wetland |
| broadleaf-cattail | 3 | 25 mm | 2.02 m | wetland |
| bulrush | 3 | 25 mm | 1.48 m | wetland |
| common-cottongrass | 3 | 25 mm | 0.47 m | wetland |
| reed-sweet-grass | 3 | 25 mm | 1.07 m | wetland |
| soft-rush | 3 | 25 mm | 0.88 m | wetland |
| sweet-flag | 3 | 25 mm | 0.68 m | wetland |
| water-horsetail | 3 | 25 mm | 0.85 m | wetland |
| water-reed | 3 | 25 mm | 1.2 m | wetland |
| wild-rice | 3 | 25 mm | 0.75 m | wetland |

### grass — 38 species, 114 variants

| species | variants | pitch | max h | cover role |
|---|---|---|---|---|
| bilberry-mat | 3 | 25 mm | 0.4 m | shade-shrub |
| blanket-weed | 3 | 25 mm | 0.1 m | wetland |
| bracken | 3 | 25 mm | 0.8 m | sun |
| brook-moss-cushion | 3 | 25 mm | 0.12 m | shade |
| canadian-waterweed | 3 | 25 mm | 0.33 m | wetland |
| cocksfoot | 3 | 25 mm | 0.6 m | sun |
| common-stonewort | 3 | 25 mm | 0.35 m | wetland |
| curled-pondweed | 3 | 25 mm | 0.33 m | wetland |
| dogs-mercury | 3 | 25 mm | 0.38 m | shade |
| feather-moss | 3 | 25 mm | 0.07 m | shade |
| hair-cap-moss | 3 | 25 mm | 0.28 m | shade |
| harts-tongue-fern | 3 | 25 mm | 0.42 m | shade |
| ivy-ground-layer | 3 | 25 mm | 0.1 m | shade |
| jungle-groundcover | 3 | 25 mm | 0.33 m | shade |
| lady-fern | 3 | 25 mm | 0.6 m | shade |
| lesser-pond-sedge | 3 | 25 mm | 0.55 m | wetland |
| male-fern | 3 | 25 mm | 1.0 m | shade |
| meadow-grass | 3 | 25 mm | 0.38 m | sun |
| moss-cushion | 3 | 25 mm | 0.2 m | shade |
| needle-spike-rush | 3 | 25 mm | 0.2 m | wetland |
| quillwort | 3 | 25 mm | 0.2 m | wetland |
| ribwort-plantain | 3 | 25 mm | 0.4 m | sun |
| rigid-hornwort | 3 | 25 mm | 0.35 m | wetland |
| sedge-tussock | 3 | 25 mm | 0.47 m | wetland |
| sphagnum-hummock | 3 | 25 mm | 0.2 m | wetland |
| spiked-water-milfoil | 3 | 25 mm | 0.42 m | wetland |
| sword-fern | 3 | 25 mm | 1.07 m | shade |
| timothy | 3 | 25 mm | 0.65 m | sun |
| understory-fern | 3 | 25 mm | 0.6 m | shade |
| water-chestnut | 3 | 25 mm | 0.05 m | wetland |
| water-earwort | 3 | 25 mm | 0.03 m | wetland |
| water-soldier | 3 | 25 mm | 0.25 m | wetland |
| water-starwort | 3 | 25 mm | 0.03 m | wetland |
| watercress | 3 | 25 mm | 0.28 m | wetland |
| willow-moss | 3 | 25 mm | 0.12 m | wetland |
| wood-horsetail | 3 | 25 mm | 0.42 m | shade |
| wood-sedge | 3 | 25 mm | 0.5 m | shade |
| woolly-fringe-moss | 3 | 25 mm | 0.1 m | shade |

### flower — 45 species, 135 variants

| species | variants | pitch | max h | cover role |
|---|---|---|---|---|
| arrowhead | 3 | 25 mm | 0.55 m | wetland |
| birdsfoot-trefoil | 3 | 25 mm | 0.28 m | sun |
| bogbean | 3 | 25 mm | 0.4 m | wetland |
| bugle | 3 | 25 mm | 0.25 m | shade |
| common-bluebell | 3 | 25 mm | 0.3 m | spring-woodland |
| common-dog-violet | 3 | 25 mm | 0.12 m | shade |
| common-knapweed | 3 | 25 mm | 0.42 m | sun |
| common-milkweed | 3 | 25 mm | 0.62 m | sun |
| common-poppy | 3 | 25 mm | 0.68 m | sun |
| cowslip | 3 | 25 mm | 0.3 m | sun |
| cyclamen | 3 | 25 mm | 0.12 m | shade |
| field-scabious | 3 | 25 mm | 0.35 m | sun |
| fireweed | 3 | 25 mm | 0.55 m | sun |
| flowering-rush | 3 | 25 mm | 0.72 m | wetland |
| foxglove | 3 | 25 mm | 1.48 m | sun |
| fringed-water-lily | 3 | 25 mm | 0.05 m | wetland |
| harebell | 3 | 25 mm | 0.17 m | sun |
| hellebore | 3 | 25 mm | 0.47 m | shade |
| herb-robert | 3 | 25 mm | 0.4 m | shade |
| impatiens | 3 | 25 mm | 0.42 m | sun |
| jungle-understory-flower | 3 | 25 mm | 0.68 m | shade |
| large-trillium | 3 | 25 mm | 0.38 m | spring-woodland |
| lily-of-the-valley | 3 | 25 mm | 0.25 m | spring-woodland |
| marsh-cinquefoil | 3 | 25 mm | 0.3 m | wetland |
| marsh-marigold | 3 | 25 mm | 0.35 m | wetland |
| meadow-daisy | 3 | 25 mm | 0.2 m | sun |
| oxeye-daisy | 3 | 25 mm | 0.62 m | sun |
| pickerelweed | 3 | 25 mm | 0.6 m | wetland |
| prairie-lupine | 3 | 25 mm | 0.62 m | sun |
| primrose | 3 | 25 mm | 0.23 m | spring-woodland |
| purple-loosestrife | 3 | 25 mm | 0.53 m | wetland |
| ramsons | 3 | 25 mm | 0.47 m | spring-woodland |
| red-campion | 3 | 25 mm | 0.7 m | shade |
| red-clover | 3 | 25 mm | 0.38 m | sun |
| river-water-crowfoot | 3 | 25 mm | 0.07 m | wetland |
| trout-lily | 3 | 25 mm | 0.23 m | spring-woodland |
| water-forget-me-not | 3 | 25 mm | 0.28 m | wetland |
| water-mint | 3 | 25 mm | 0.3 m | wetland |
| water-plantain | 3 | 25 mm | 0.53 m | wetland |
| white-water-lily | 3 | 25 mm | 0.05 m | wetland |
| wood-anemone | 3 | 25 mm | 0.23 m | spring-woodland |
| wood-sorrel | 3 | 25 mm | 0.15 m | shade |
| yarrow | 3 | 25 mm | 0.38 m | sun |
| yellow-flag-iris | 3 | 25 mm | 0.88 m | wetland |
| yellow-water-lily | 3 | 25 mm | 0.05 m | wetland |

## Scope

This counts the species one fixture places. It is not a claim about the whole asset library, which holds many
more species (animals among them) that this slice never places. It is also not a quality judgement: every one of
these passed reference review, which is why they are candidates at all.
