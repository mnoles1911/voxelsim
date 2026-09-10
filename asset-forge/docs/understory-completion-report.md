# Temperate understory completion report

Completed: 113 reviewed authoritative sources and 4,068 deterministic variants, 36 per profile. All use 25 mm cubic voxels and spring appearance. Source seed 7 is the authoritative reference instance. All variants remain pending user endorsement; source acceptance does not approve world placement.

Scope includes 20 bushes, 10 reeds, 38 grass-category profiles and 45 flowers. Existing grass categories also contain ferns, mosses and aquatic plants. Wetland and aquatic species require their correct biome microhabitats. The two jungle-labelled legacy IDs are retained with documented temperate analogues: Great wood-rush (Luzula sylvatica) and False Solomon's seal (Maianthemum racemosum).

## Review and pipeline

Actual remote botanical photographs were inspected, followed by small/medium/large pilots (seeds 1, 4 and 7) and all 36-variant contact sheets. Per-profile records bind acceptance to generator identity, image sheets and generated artifacts. Refactors distinguish woody branch systems, cane shrubs, fronds, basal fans, creeping runners, rosettes, moss colonies, floating pads and submerged whorled shoots. Summer-flowering species use vegetative spring forms; limited late-spring blooms are recorded individually.

Authoritative baseline, current generator digest, reference ID and seed inventory live in `library/<species>/species.json`; photo observations and geometry findings are in `reference-review.json` beside it. Pending artifact bytes live in `out/forge-candidates/<species>/<variant>/`. Forge and Asset Library use these existing persistent records. The 1.8 m scale pawn remains available in the preview. No global game banks were published by this collection pass.

Generator dependency isolation preserves all 49 previously accepted tree sources. Relevant architecture/helper edits invalidate affected understory sources; unrelated profile edits preserve their identity. Identity migrations were allowed only after rebuilding saved voxels to identical bytes. User decisions and endorsed geometry were preserved.

## Validation and retirement

- All 113 sources have current approved generator identities, available seed-7 references, spring baselines and 36 distinct 25 mm VXA geometries.
- Artifact hashes and deterministic rebuilds passed. Independent root-agent payload checks passed all 4,068 VXA files; 24 explicitly vegetative profiles also passed leaf/bark-only material checks.
- Fresh-session endorsement/rejection persistence and stale/corrupt cache guards passed. Existing 686 unrelated recipe hashes and 49 accepted tree sources remain valid.
- Active inventory sweep found exactly 4,068 scoped variants, zero old active assets and zero orphan VXA folders.
- Twelve pre-review assets were retired after replacements passed: three each for meadow-grass, bramble-thicket, meadow-daisy and water-reed. No protected endorsed/rejected asset was removed. Historical review evidence remains offline as provenance.
- Final port-8731 HTTP audit passed all 113 current sources and reference availability after server reload.

## Cost and limitations

No paid generation API calls were used. Latest recorded per-profile generation batches sum to 631.96 seconds (about 10.5 minutes); this excludes Astra research/review time, earlier iterations and elapsed wall time. This is a CPU geometry pipeline, not an image-generation-per-variant workflow.

25 mm cannot resolve subvoxel leaves, fern pinnules, fine grass blades, hairs or tiny flowers faithfully. These remain coarse connected colonies or silhouettes at correct physical scale; plant organs were not enlarged to conceal this limit. Small flowers and dense leaves can merge. No new transparency or flower-cutout exception was introduced. Root connectors must embed into soil/sediment; floating plants need waterline alignment and submerged species need appropriate placement depth. No automatic world placement or biome-density tuning is claimed by this pass.

Remote reference photos remain remotely hosted. Some host-signed image URLs expire; stable source-page links and inspection observations are retained. Refresh from the credited source when required.

## Review artifacts

- [All sources, photos, pilots and sheets](http://127.0.0.1:8731/static/understory-review/index.html)
- `out/understory-review/manual-findings.json`: artifact-bound visual decisions
- `out/understory-collection/audit-report.json`: per-profile distinctness, pitch, timing and hashes
- `out/understory-collection/active-inventory-audit.json`: old/orphan asset sweep
- `out/understory-review/live-audit.json`: final application visibility
- `out/understory-collection/*-retirement.json`: contained retirement provenance

## Completed profile inventory

Each row has 36 seeds, seed 7 as reference, 25 mm pitch, and pending variant endorsements.

| Profile | Botanical reference | Architecture |
|---|---|---|
| arrowhead | Sagittaria latifolia | sagittaria |
| bilberry-mat | Vaccinium myrtillus | dwarf |
| birdsfoot-trefoil | Lotus corniculatus | lotus |
| blackthorn-scrub | Prunus spinosa | shrub |
| blanket-weed | Cladophora glomerata | cladophora |
| bogbean | Menyanthes trifoliata | menyanthes |
| box | Buxus sempervirens | evergreen |
| bracken | Pteridium aquilinum | fern |
| bramble-thicket | Rubus fruticosus | cane |
| branched-bur-reed | Sparganium erectum | sparganium |
| broadleaf-cattail | Typha latifolia | cattail |
| brook-moss-cushion | Brachythecium rivulare | rivulare |
| bugle | Ajuga reptans | ajuga |
| bulrush | Schoenoplectus lacustris | schoenoplectus |
| butchers-broom | Ruscus aculeatus | evergreen |
| canadian-waterweed | Elodea canadensis | elodea |
| cocksfoot | Dactylis glomerata | dactylis |
| common-bluebell | Hyacinthoides non-scripta | bells |
| common-broom | Cytisus scoparius | scoparius |
| common-cottongrass | Eriophorum angustifolium | eriophorum |
| common-dog-violet | Viola riviniana | violet |
| common-dogwood | Cornus sanguinea | shrub |
| common-knapweed | Centaurea nigra | centaurea |
| common-milkweed | Asclepias syriaca | asclepias |
| common-poppy | Papaver rhoeas | papaver |
| common-stonewort | Chara vulgaris | chara |
| cowslip | Primula veris | cowslip |
| curled-pondweed | Potamogeton crispus | potamogeton |
| cyclamen | Cyclamen hederifolium | hederifolium |
| dog-rose | Rosa canina | cane |
| dogs-mercury | Mercurialis perennis | opposite |
| elder | Sambucus nigra | compound |
| feather-moss | Pleurozium schreberi | feathermoss |
| field-scabious | Knautia arvensis | knautia |
| fireweed | Chamaenerion angustifolium | chamaenerion |
| flowering-rush | Butomus umbellatus | butomus |
| foxglove | Digitalis purpurea | digitalis |
| fringed-water-lily | Nymphoides peltata | nymphoides |
| gorse | Ulex europaeus | ulex |
| guelder-rose | Viburnum opulus | viburnum |
| hair-cap-moss | Polytrichum commune | haircap |
| harebell | Campanula rotundifolia | campanula |
| harts-tongue-fern | Asplenium scolopendrium | strap |
| hazel-coppice | Corylus avellana | coppice |
| hellebore | Helleborus orientalis | helleborus |
| herb-robert | Geranium robertianum | geranium |
| holly-understorey | Ilex aquifolium | evergreen |
| impatiens | Impatiens capensis | impatiens |
| ivy-ground-layer | Hedera helix | hedera |
| jungle-groundcover | Luzula sylvatica | woodrush |
| jungle-understory-flower | Maianthemum racemosum | panicleherb |
| lady-fern | Athyrium filix-femina | fern |
| large-trillium | Trillium grandiflorum | trillium |
| lesser-pond-sedge | Carex acutiformis | acutiformis |
| lily-of-the-valley | Convallaria majalis | convallaria |
| male-fern | Dryopteris filix-mas | fern |
| marsh-cinquefoil | Comarum palustre | comarum |
| marsh-marigold | Caltha palustris | caltha |
| meadow-daisy | Bellis perennis | bellis |
| meadow-grass | Poa pratensis | grass |
| moss-cushion | Leucobryum glaucum | moss |
| mountain-laurel | Kalmia latifolia | kalmia |
| needle-spike-rush | Eleocharis acicularis | eleocharis |
| oxeye-daisy | Leucanthemum vulgare | leucanthemum |
| pickerelweed | Pontederia cordata | pontederia |
| prairie-lupine | Lupinus perennis | lupinus |
| primrose | Primula vulgaris | rosette |
| purple-loosestrife | Lythrum salicaria | lythrum |
| quillwort | Isoetes lacustris | isoetes |
| ramsons | Allium ursinum | wildgarlic |
| red-campion | Silene dioica | silene |
| red-clover | Trifolium pratense | trifolium |
| red-huckleberry | Vaccinium parvifolium | dwarf |
| reed-sweet-grass | Glyceria maxima | glyceria |
| rhododendron-thicket | Rhododendron ponticum | ponticum |
| ribwort-plantain | Plantago lanceolata | plantago |
| rigid-hornwort | Ceratophyllum demersum | ceratophyllum |
| river-water-crowfoot | Ranunculus fluitans | fluitans |
| salal | Gaultheria shallon | evergreen |
| sedge-tussock | Carex cespitosa | carexcespitosa |
| snowberry | Symphoricarpos albus | shrub |
| soft-rush | Juncus effusus | juncus |
| sphagnum-hummock | Sphagnum palustre | peatmoss |
| spiked-water-milfoil | Myriophyllum spicatum | myriophyllum |
| spindle | Euonymus europaeus | shrub |
| sweet-flag | Acorus calamus | acorus |
| sword-fern | Polystichum munitum | fern |
| timothy | Phleum pratense | phleum |
| trout-lily | Erythronium americanum | erythronium |
| understory-fern | Dryopteris filix-mas | fern |
| water-chestnut | Trapa natans | trapa |
| water-earwort | Salvinia natans | salvinia |
| water-forget-me-not | Myosotis scorpioides | myosotis |
| water-horsetail | Equisetum fluviatile | fluviatile |
| water-mint | Mentha aquatica | mentha |
| water-plantain | Alisma plantago-aquatica | alisma |
| water-reed | Phragmites australis | phragmites |
| water-soldier | Stratiotes aloides | stratiotes |
| water-starwort | Callitriche stagnalis | callitriche |
| watercress | Nasturtium officinale | nasturtium |
| white-water-lily | Nymphaea alba | nymphaea |
| wild-privet | Ligustrum vulgare | shrub |
| wild-rice | Zizania aquatica | zizania |
| willow-moss | Fontinalis antipyretica | fontinalis |
| witch-hazel | Hamamelis virginiana | shrub |
| wood-anemone | Anemone nemorosa | anemone |
| wood-horsetail | Equisetum sylvaticum | sylvaticum |
| wood-sedge | Carex sylvatica | carexsylvatica |
| wood-sorrel | Oxalis acetosella | oxalis |
| woolly-fringe-moss | Racomitrium lanuginosum | lanuginosum |
| yarrow | Achillea millefolium | achillea |
| yellow-flag-iris | Iris pseudacorus | pseudacorus |
| yellow-water-lily | Nuphar lutea | nuphar |
