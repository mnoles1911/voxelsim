# Taiga — master species list

**Biome id 7** — `vxc::BiomeId::TAIGA`, `voxel-core/include/voxelcore/biome.h:31`; mirrored `asset-forge/forge/biomes.py:71`.

| | |
|---|---|
| Climate envelope | mean annual temperature below 5 C (`biome.h:225`). That test runs **before any precipitation test**, so taiga is a pure temperature biome and spans the full moisture range — dry cold forest and waterlogged bog are both taiga. |
| Surface material | podzol (`MAT_PODZOL`, `biome.h:248`) |
| Share of land (shipped world) | **6.64%** over 289 tiles, best tile `-11_-6` at 83.2% (`docs/measurements/biome-screenshot-targets-2026-08-01.txt`). An earlier 121-tile pass in the same file read 6.74% with a best tile of 66.3%; **use the 289-tile column**, which that file's own two corrections say supersedes it. |
| Water present | **Yes, and plenty.** Cold fast rivers, bog pools, peat hags and lakes. A river's biome is the biome of the land around it (`biomes.py:37-42`), so a fish in a taiga river is tagged taiga. |
| Asset kinds hosted | tree, bush, rock, grass, reed, flower, fish, cetacean, bird (`biomes.py:71-72`) — everything |

Two facts about the gate order shape this whole list. First, taiga is decided on temperature alone and before precipitation, which is why the plant list has to cover both a dry lichen-floored pine forest and a saturated sphagnum bog — those are the same biome here, not two. Second, `biome.h:216-221` runs sea level, the cliff gate and the treeline **before** the climate table, and the treeline is 900 m at 0 C plus about 150 m per degree C (`biome.h:98-99`). So taiga is the cold forest strictly **below** that line; anything above it is TUNDRA_ALPINE and belongs in that file. A taiga asset will never be placed on a summit, and nothing on this page should be designed as if it might be.

Taiga is genuinely interleaved — it sits between tundra above it and grassland below it — so most species here share a shot with a neighbouring biome's assets and have to be distinguishable from them. **But do not overstate it.** The 121-tile census made taiga look as though it never filled a frame, at a 66.3% best tile; on 289 tiles the best tile is 83.2%, and the census file records that every biome now has a dominant tile. Taiga can be the whole shot.

## How to read the tables

**Status** is one of:
* `shipped: <spec-name>` — a spec already exists in `asset-forge/specs/`.
* `queued` — a generator exists for this kind; this is authoring work only.
* `gen: <name>` — needs a generator that does not exist. See the index for the gap list.

**Lattice** (animal tables only) is the voxel size the asset should be authored at, by the house rule in `asset-forge/forge/kinds.py:29-58` and `asset-forge/docs/marine-megafauna-research.md:364-365`: a species is drawn at the COARSEST voxel size at which its smallest identifying feature is still about three voxels across. Trees and rocks are not listed with a lattice because they join the world's terrain grid and are 10 cm and nothing else (`kinds.py:34-42`); ground cover, bushes, flowers and reeds are 5 cm.

**⚠** marks a species whose defining feature is at or below what the lattice will hold, explained in the note under its table.

## Trees

Two of the six shipped trees tagged to taiga are marginal on range and the file should say so rather than smooth it over. `hero-sequoia` at 80 m is a Sierra montane species, not a boreal one — it is here as the biome's landmark canopy tree, and that is an art decision, not a biogeographic one. `columnar-cypress` is Mediterranean and is doing the same job for a dark narrow spire. Both are fine to keep; neither should be cited as evidence of what a boreal forest contains.

`alpine-krummholz` is the opposite case — it is exactly right, but note where it goes: krummholz is the deformed growth form **at** the treeline, so it belongs at the top edge of taiga placement, right where `biome.h:221` hands off to TUNDRA_ALPINE, and nowhere else.

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Birch | Bright white papery trunk marked with black horizontal lenticel dashes and dark diamonds at old branch scars, slender, with a light open crown of small triangular leaves. | 16 | `shipped: birch` |
| Tundra pine | Short thick trunk, wide-spaced whorls of stiff level branches, dark blue-green needle clumps concentrated at the branch ends leaving the inner branch bare. | 9 | `shipped: tundra-pine` |
| Alpine krummholz | Wind-deformed low mat: trunk bent to the horizontal, all branches streaming to one side, dead bleached wood on the windward face and green only on the lee. | 1.8 | `shipped: alpine-krummholz` |
| Columnar cypress | Very narrow dark green vertical spire, near-parallel sides, no visible trunk at all — a column of dense foliage. Marginal: Mediterranean, kept for silhouette variety. | 13 | `shipped: columnar-cypress` |
| Sapling | Thin straight single stem with three or four short side branches and a small sparse crown; reads as a stick with leaves. | 4.5 | `shipped: temperate-sapling` |
| Giant sequoia | Enormously thick fluted cinnamon-red fibrous trunk, bare for most of its height, with a rounded crown of short blunt branches far above. Marginal: not boreal — the biome's hero landmark. | 80 | `shipped: hero-sequoia` |
| Norway spruce | Perfect narrow cone from ground to a single spike tip, branches level below and sweeping down at the ends, very dark green, with long pendulous cones hanging from the upper branches. | 35 | `queued` (tree, whorl) |
| Siberian larch | Conifer shape but **deciduous**: open conical crown of soft light-green needle tufts in summer, brilliant gold in autumn, bare grey branch skeleton in winter — the only conifer here that ever shows bare. | 30 | `queued` (tree, whorl) |
| Scots pine | Straight bare lower trunk in flaking orange-red plates on the upper third and grey-brown below, with an irregular flat-topped crown of blue-green needles held only at the very top. | 25 | `queued` (tree, whorl) |

## Rock types

Eleven rock specs are tagged taiga; seven are tabled here. The other four — `corestone-tor`, `jointed-granite-tor`, `summit-tor` (3.2 m) and `hero-tor-stack` (4.0 m) — are the same jointed-granite tor landform at four scales, and their description is the `granite-boulder` and `standing-stone` rows below assembled into stacked blocks.

Everything on this list is glacial or frost-weathering country, which is correct: the taiga band is where ice sheets were, and the rocks should read as delivered and broken rather than as bedrock in place.

| Rock | Voxel-artist description | Block size (m) | Status |
|---|---|---|---|
| Granite boulder | Rounded blocky grey boulder with coarse speckled black-and-white grain, corners knocked off but faces still flat, part-buried at one end. | 1.6 | `shipped: granite-boulder` |
| Glacial erratic | A boulder of visibly the wrong rock for its surroundings, sitting isolated on open ground with no talus and no outcrop near it; the *isolation* is the asset. | 2.0 | `shipped: glacial-erratic` |
| Mossy forest boulder | Rounded boulder completely capped in thick bright green moss over the top and north face, bare wet grey stone below the moss line, often with a seedling rooted on top. | 1.5 | `shipped: mossy-forest-boulder` |
| Alpine scree | A slope-covering field of angular flat plates, all of one rock type, roughly sorted with the largest at the bottom; no rounding anywhere. | 0.4 per plate | `shipped: alpine-scree` |
| Cliff-fall block | Large sharp-edged rectangular block with clean fracture faces still bright, tilted at an angle, sitting alone at the foot of a slope. | 2.8 | `shipped: cliff-fall-block` |
| Standing stone | Tall narrow slab set on end, height roughly three times its width, flat-faced with rounded top, lichen-crusted on the exposed faces. | 2.2 | `shipped: standing-stone` |
| Veined granite | Grey granite cut by two or three sharp straight white quartz veins running clean across all faces at an angle to each other. | 1.5 | `shipped: veined-granite` |

## Flowers

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Alpine cushion flower | Tight low hemispherical cushion of tiny leaves with small flowers sitting flush on the surface, no stems visible at all. | 0.24 | `shipped: alpine-cushion-flower` |
| Fireweed | Single tall unbranched stem with narrow leaves in a spiral, topped by a long spike of magenta-pink flowers that open from the bottom upward — so the spike is flowers below, buds above. | 1.2 | `queued` |
| Twinflower | Creeping evergreen mat with hair-thin upright Y-forked stems, each fork carrying two nodding pale pink bells; the paired bells are the identity. | 0.08 | `queued` |
| Marsh marigold | Glossy dark-green kidney-shaped leaves in a low clump at the water's edge, with large flat waxy golden-yellow cups held just above them. | 0.3 | `queued` |
| Cloudberry | Low sparse plant with a few broad crinkled lobed leaves and one white flower per stem, becoming a single amber-orange raspberry-like fruit; bog surfaces only. | 0.2 | `queued` |

## Ground cover

Reeds live in this section rather than their own; the cottongrass row is the reed kind and belongs at bog and lake margins.

**Fungi are a real part of a boreal forest floor and there is no generator for them.** One entry below carries `gen: fungus`, but the request covers two shapes: a stalked cap (the fly agaric row) and a bracket — a smooth pale hoof-shaped shelf growing straight out sideways from a birch trunk with no stalk at all, flat underneath, in stacked groups, about 0.2 m across. The tuft generator behind grass, reeds and flowers (`kinds.py:85-96`) makes a spray of thin stems from a root crown, which is the wrong topology for both: a mushroom is one thick stalk carrying one solid dome, and a bracket has no stalk.

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Alpine sedge | Stiff narrow three-ranked blades standing near-vertical from a tight base, dull grey-green, much stiffer and straighter than a grass. | 0.3 | `shipped: alpine-sedge` |
| Meadow grass | Soft arching green blades from a loose crown with a few thin flowering stems held above the leaf mass. | 0.45 | `shipped: meadow-grass` |
| Reindeer lichen | Pale grey-green spongy branching mass with no stems and no leaves — repeatedly forked hollow tubes building a low springy cushion; forms continuous sheets on dry pine floor. | 0.08 | `queued` (grass generator, heavy forking) |
| Red-stemmed feather moss | Continuous deep-green carpet of feathered fronds layered like stepped shingles, red stems just visible beneath; covers the ground unbroken between trunks. | 0.06 | `queued` |
| Sphagnum | Saturated pale hummock of tight star-shaped rosettes, colours ranging from lime green through ochre to deep red in patches on the same surface; builds domed mounds, not a flat mat. | 0.2 mound | `queued` |
| Common cottongrass | Stiff grass-like tuft topped by a single pure white silky cotton-ball head on each stem, standing above wet bog; the white heads are the whole visual. | 0.5 | `queued` (reed) |
| Fly agaric | Single thick white stalk with a ring collar and a bulbous base, carrying one domed scarlet cap covered in scattered raised white flecks. The flecks are 5-10 mm and will not survive 5 cm — author them as flat colour, not geometry. | 0.15 | `gen: fungus` |

## Bushes / shrubs

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Bramble thicket | Dense impassable tangle of long arching thorny canes rooting where the tips touch ground, dark green leaves and no clear outline. | 1.5 | `shipped: bramble-thicket` |
| Juniper scrub | Low sprawling dark blue-green mound of very short prickly needles held in dense sprays, spreading wider than tall, with small blue-grey berries. | 1.1 | `shipped: juniper-scrub` |
| Labrador tea | Upright open evergreen shrub with narrow leathery leaves whose edges roll under, dark above and rusty-woolly beneath, topped by flat clusters of small white flowers; bog margins. | 0.8 | `queued` |
| Bilberry | Low dense green mound of small oval leaves on sharply angled green twigs, forming a continuous knee-high understorey layer beneath pines; turns crimson in autumn. | 0.4 | `queued` |
| Dwarf birch | Ankle-high woody mat with tiny round scalloped leaves on stiff dark twigs, spreading horizontally; the leaves are 1 cm and read as texture, not shape. | 0.5 | `queued` |

## Birds

Eleven of these twelve already ship, which makes birds by far the best-covered kind in this biome and the reason the build priority below puts nothing bird-shaped near the top.

Two shipped specs carry a deliberate size discrepancy and it must not be "corrected": **`european-robin` is authored at 0.24 m against a real bird of about 0.14 m, and `great-tit` likewise.** The reason is in the pose — `specs/european-robin.json` sets `bird.posture_deg` and `bird.neck_up_deg` to 42, and a perched songbird tilted 42 degrees nose-up projects onto too few voxels at life size to have a shape at 1 cm. Authoring above life size and recording it is the project's standard fix for that, and it is the same fix recommended for several animals in the desert file.

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Golden eagle | Very large dark brown raptor with a distinctly paler golden nape, long broad wings held in a shallow V with fingered tips, and a heavy hooked yellow-based bill. | 0.85 | 1 cm | `shipped: golden-eagle` |
| Common raven | Heavy all-black bird, thick deep bill, shaggy throat feathers and a long wedge-shaped tail. | 0.64 | 1 cm | `shipped: common-raven` |
| Mallard duck | Broad flat body low to the water, orange legs set far back; drake has a bottle-green head, thin white neck ring, grey body and a curled black tail feather. | 0.58 | 1 cm | `shipped: mallard-duck` |
| Common buzzard | Medium raptor, broad rounded wings, short fanned tail, and a pale U-shaped band across a dark brown breast; very variable overall tone. | 0.52 | 1 cm | `shipped: common-buzzard` |
| Tawny owl | Round-headed owl with no ear tufts, a large flat facial disc, black eyes and mottled brown bark-camouflage plumage; the disc and the black eyes are the identity. | 0.40 | 1 cm | `shipped: tawny-owl` |
| Rock ptarmigan | Compact ground grouse, pure white in winter with a black eye stripe on the male and feathered white feet; mottled grey-brown in summer. | 0.36 | 1 cm | `shipped: rock-ptarmigan` |
| Eurasian jay | Pink-buff body, black moustache stripe, white rump against a black tail, and a small brilliant barred blue panel on the wing shoulder. ⚠ | 0.34 | 1 cm | `shipped: eurasian-jay` |
| European robin | Round brown bird with an orange-red face and breast bordered grey, thin legs, upright perched posture. Authored at 0.24 m against a real ~0.14 m — see the note above. | 0.24 (real ~0.14) | 1 cm | `shipped: european-robin` |
| Great tit | Black head with clean white cheek ovals, yellow underside split by a broad black stripe from throat to belly, blue-green back. Authored at 0.24 m against a real ~0.14 m — same reason. | 0.24 (real ~0.14) | 1 cm | `shipped: great-tit` |
| Song thrush | Warm brown above, cream below covered in neat dark arrowhead spots arranged in rows; upright stance, short bill. | 0.23 | 1 cm | `shipped: song-thrush` |
| Great spotted woodpecker | Black-and-white pied bird clinging vertically to a trunk, bold white shoulder ovals, crimson patch under the tail, stiff tail pressed to the bark as a prop. | 0.23 | 1 cm | `shipped: great-spotted-woodpecker` |
| Western capercaillie | Turkey-sized black grouse, heavy body, pale ivory bill, red skin above the eye, shaggy black throat beard, and a broad black tail fanned upright in display. | 0.9 | 2 cm | `queued` |

⚠ **Eurasian jay** — the blue wing panel is finely barred black and blue, and each bar is a few millimetres. At 1 cm the barring cannot exist. Author the panel as a solid blue block with two or three dark bars at whatever spacing the lattice allows and accept it as a symbol; this is already how the shipped spec handles it.

## Land animals

**There is no quadruped generator.** Fifteen of the eighteen rows below need one. This is the same blocker as every other biome file, and it is the single largest gap in the library.

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| Moose | Enormously tall at the shoulder with a humped withers, long pale legs, a heavy pendulous overhanging muzzle and a hanging throat dewlap; bulls carry broad flat palmate antlers spread wide. | 2.7 long / 1.9 shoulder | 5 cm | `gen: quadruped` |
| Reindeer | Stocky deer with a thick pale neck ruff, short muzzle, broad splayed hooves, and long asymmetric antlers with a forward-projecting flattened brow tine over the face. ⚠ | 1.8 long / 1.1 shoulder | 2 cm | `gen: quadruped` |
| Red deer stag | Rich red-brown body with a heavy maned neck, and tall multi-branched antlers of many separate round tines sweeping up and back — the antlers are most of the silhouette. ⚠ | 2.0 long / 1.3 shoulder | 2 cm | `gen: quadruped` |
| Siberian musk deer | Small hunched deer with no antlers at all, arched back, hindquarters far higher than shoulders, and two long downward-curving tusks projecting from the upper jaw past the chin. ⚠ | 0.85 long / 0.55 shoulder | 1 cm | `gen: quadruped` |
| Wood bison | Massive triangular forequarter hump much taller than the hips, huge low-carried head, shaggy dark brown cape over shoulders and forelegs against a shorter smooth rear, short pale horns curving out then up. The European bison is the same build with a higher head carriage and a shorter cape. | 2.9 long / 1.8 shoulder | 5 cm | `gen: quadruped` |
| Wild boar | Wedge-shaped body, high shoulders sloping to low hips, coarse dark bristle crest along the spine, long straight snout and short upcurved tusks at the mouth corners. | 1.4 long / 0.85 shoulder | 2 cm | `gen: quadruped` |
| Brown bear | Bulky with a pronounced muscular shoulder hump, dished concave face profile, small round ears set wide and low, and a very short tail; walks flat-footed on broad paws. | 2.0 long / 1.0 shoulder | 5 cm | `gen: quadruped` |
| Grey wolf | Long-legged deep-chested dog with a narrow chest, straight bushy tail carried level, grizzled grey saddle over a pale chest and legs, erect triangular ears. | 1.2 long / 0.8 shoulder | 2 cm | `gen: quadruped` |
| Eurasian lynx | Long-legged short-bodied cat with an obviously bobbed black-tipped tail, flared cheek ruff, and tall black ear tufts. ⚠ | 1.0 long / 0.65 shoulder | 2 cm | `gen: quadruped` |
| Wolverine | Low heavy-set weasel built like a small bear: broad head, short powerful legs, huge feet, dark brown with a pale band running along each flank from shoulder to a bushy tail. | 0.85 long / 0.42 shoulder | 2 cm | `gen: quadruped` |
| Red fox | Slender rust-orange dog with black stockings, large pointed ears, a sharp narrow muzzle and a very thick white-tipped brush tail carried low and straight out. | 0.7 long / 0.4 shoulder | 2 cm | `gen: quadruped` |
| Mountain hare | Compact hare with shorter ears than a brown hare, ears black-tipped, very long hind legs, and a fully white winter coat. ⚠ | 0.55 long / 0.3 shoulder | 1 cm | `gen: quadruped` |
| Red squirrel | The tail is the silhouette: a long plume held arched over the back, as long as the body, plus tall pointed ear tufts in winter; rust-red body with a white belly. ⚠ | 0.22 body / 0.18 tail | 1 cm | `gen: quadruped` |
| Siberian flying squirrel | Small grey squirrel with enormous black eyes and a loose skin membrane between wrist and ankle that makes it a flat rectangle when spread and a baggy fold when not; flattened tail. ⚠ | 0.16 body | 1 cm | `gen: quadruped` |
| Eurasian beaver | Heavy low barrel body, tiny ears, dark brown, and a broad flat horizontally-held scaly paddle tail — the tail is unmistakable and nothing else needs to be. | 0.9 body / 0.35 tail | 2 cm | `gen: quadruped` |
| Wood ant | Reddish-brown thorax with a dark head and gaster, on a domed mound of pine needles up to a metre high; the mound is the placeable asset, the ant is detail on it. ⚠ | 0.01 / mound 1.0 | 1 cm | `gen: arthropod` |
| European adder | Short thick grey or brown snake carrying a continuous bold dark zigzag stripe down the whole back, a V or X mark on the head, and a vertical pupil. | 0.6 total | 1 cm | `gen: serpentine` |
| Painted turtle | Smooth flat dark olive shell with red bars around the rim and yellow stripes on the head and neck, basking flat on a log. **Marginal** — reaches only the southern edge of the boreal zone; include only if the world wants southern-taiga lake life. | 0.2 shell | 1 cm | `gen: chelonian` |

**`gen: serpentine` is probably not a new generator.** The fish generator already lofts a single solid whose cross-section varies along one axis, and an anguilliform body is inside its existing range: `asset-forge/docs/fish-shape-research.md:53` records a median length-to-depth ratio of 16.8 for eel-like fish, and the shipped `specs/river-eel.json` is authored at `fish.length_m` 0.70 with `fish.depth_ratio` 0.085, about 12:1. An adder at 0.6 m and roughly 3.5 cm through is 17:1 — within that. Turn every fin off, keep the loft, and the snake clade is closer to shipped than any mammal on this page. Worth testing before the quadruped is scoped, because if it is a day's work the build order changes.

⚠ notes for this table:
* **Red deer stag** — the antlers are the animal, and an individual tine is roughly 3-4 cm thick and tapering. At 2 cm a tine is one to two voxels, and a one-voxel antler does not read as an antler; it reads as speckle above the head. This is a different failure from the squirrel below: the squirrel's problem is that a *fine* feature chooses a *fine* lattice for the whole body, while the stag's is that the feature disappears entirely at the lattice its body size wants. The honest fix is to thicken the tines to at least three voxels — deliberately heavier antlers than life — and write that into the spec notes.
* **Reindeer** — same problem as the stag, with the added constraint that the forward brow tine over the face is the mark that distinguishes it. Thicken it and keep it, even if the rear tines are simplified away.
* **Siberian musk deer** — the tusks are perhaps 7 cm long and under 1 cm thick. At 1 cm they are one-voxel spikes. They are also the only thing distinguishing this species from a small antlerless deer, so they must be thickened rather than dropped.
* **Eurasian lynx** — the black ear tufts are hair, a few millimetres across, below every lattice this project has. Author as solid blunt tapered spikes two voxels wide, as a symbol.
* **Mountain hare** — the ears carry the identity and are flat plates. Author them two voxels thick at 1 cm.
* **Red squirrel** — the tail, not the animal, chooses the lattice. The body is 22 cm and would be fine at 2 cm, but the tail plume is only about 4 cm through and its shape is the species, so 1 cm it is: 4 voxels of plume. The ear tufts will not survive at any lattice and should be authored as thickened blocks.
* **Siberian flying squirrel** — the gliding membrane is a sheet with essentially no thickness. At 1 cm it is a one-voxel plate, which is the minimum at which anything can exist at all (`marine-megafauna-research.md:341`). Author it at two voxels and only in the spread pose; a folded membrane is not representable and should just be modelled as a slightly baggy flank.
* **Wood ant** — an ant is 1 cm long, which is one voxel. It cannot be authored. What is placeable is the **mound**, which is a metre-high dome of pine needles and a genuinely characteristic taiga forest-floor object. Build the mound (the rock or tuft generator can do it) and drop the insect. `gen: arthropod` is listed for completeness, not as a recommendation to build it here.

## Fish

Taiga has real water and a real fish list, which makes it the opposite of the desert file. Three specs already ship. Note again that the engine tags these by the surrounding land (`biomes.py:37-42`), so a cold river crossing taiga produces taiga-weighted fish and the biome weights on these specs mean "which landscape's rivers and lakes hold this species".

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Brown trout | Torpedo body with a blunt head and a square-cut tail, olive-brown back over a golden flank scattered with black and red spots each ringed in pale halo. ⚠ | 0.30 | 1 cm | `shipped: brown-trout` |
| Northern pike | Long cylindrical body with the dorsal and anal fins set far back near the tail, a broad flat duck-like snout, olive-green with rows of pale bean-shaped spots along the flank. | 0.75 | 2 cm | `shipped: northern-pike` |
| River perch | Deep laterally compressed body with a humped back, two separate dorsal fins the first of which is spiny, five or six bold dark vertical bars, bright orange-red pelvic and anal fins. | 0.22 | 1 cm | `shipped: river-perch` |
| Arctic grayling | Slim silver-purple body carrying an enormous sail-like dorsal fin, far taller and longer than any other fin here, spotted and edged in red. The sail is the species. ⚠ | 0.4 | 1 cm | `queued` |
| Arctic char | Trout-shaped with a small head, dark blue-green back and pale spots on the flank, turning brilliant orange-red on the belly with white-edged leading rays on the lower fins. | 0.6 | 2 cm | `queued` |
| Atlantic salmon | Streamlined silver body with a small pointed head, a slightly forked tail, and sparse black X-shaped marks above the lateral line; spawning males develop a hooked lower jaw and a bronze-red flush. | 0.9 | 2 cm | `queued` |
| Burbot | The only freshwater cod: long eel-tapering body, flat wide head, a single barbel hanging from the chin, and one very long dorsal and one very long anal fin running to a rounded tail; mottled dark olive marbling. | 0.7 | 2 cm | `queued` |
| Taimen | Very large, long, blunt-headed salmonid with a big mouth, olive-grey back over a silver flank finely peppered with small dark spots, and a distinctly red tail and anal fin. | 1.5 | 2 cm | `queued` |

⚠ notes for this table:
* **Brown trout** — the haloed red spots are roughly 3-5 mm on a 30 cm fish. At 1 cm a spot is one voxel and its pale ring cannot exist. Author the spots at the lattice's minimum size, drop the halo, and accept a coarser scatter than life; the shipped spec already does this and the pattern controls in the `fish` group (`pattern_count`, `pattern_scale`) are the place to tune it.
* **Arctic grayling** — the dorsal sail is a thin membrane. `fish.fin_thick` in the shipped fish specs is an integer count of voxels, and the shipped `river-eel` uses 1. A one-voxel sail at 1 cm is acceptable because the sail's *area* is what identifies the species, not its thickness — but it must be authored tall and long enough to be unmistakable in profile, and it will look like paper from the side. That is the correct trade here.

## Build priority

1. **Norway spruce.** One spec, existing generator, and it is the single most identity-defining object in the biome — a boreal forest is a field of narrow dark spires and nothing shipped currently makes that shape. Highest visible return per unit of work on this page by a wide margin.
2. **Scots pine and Siberian larch.** Two more `queued` trees on the same whorl model. Together with the spruce they give the three real canopy silhouettes, and the larch adds the only seasonal colour change in the biome. Also: this removes any need to lean on `hero-sequoia` and `columnar-cypress` to carry taiga forests, which is the right outcome given both are marginal on range.
3. **Reindeer lichen and feather moss.** Two ground-cover specs off the shipped tuft generator. The boreal floor is a continuous pale lichen or deep green moss carpet, not soil, and podzol (`biome.h:248`) showing bare between trunks is the main thing that will make a taiga screenshot look wrong. Cheap and it fixes the whole middle ground.
4. **Bilberry and Labrador tea.** Two bush specs. Along with the ground cover above they build the layered floor-shrub-canopy structure a boreal forest actually has, and the shipped `bramble-thicket` and `juniper-scrub` are both wrong for the closed-forest interior.
5. **Arctic grayling and burbot.** Two fish on a shipped generator, both strong unusual shapes (a sail, a barbelled cod body) that no other biome will reuse. The taiga is one of the few biomes with enough water for fish to actually be seen.
6. **The quadruped generator.** Sixteen rows here wait on it, and it is the library-wide blocker. Moose and brown bear should be its first two subjects — both are large enough to author at 5 cm, so they exercise the generator without immediately hitting the antler and ear-tuft problems that the deer and lynx will.
7. **`gen: fungus`.** Small and specific, but a boreal forest floor without mushrooms is visibly missing something, and the two shapes needed (stalked cap, sideways bracket) are simple solids. Worth doing precisely because it is small.
8. **Two more birds at most.** Eleven of twelve already ship. Capercaillie is the one genuinely missing shape — a big black display grouse on the ground — and after that the bird budget for this biome is spent.

## Where the numbers come from

**Every species size on this page is an approximation from general knowledge. Nothing here is measured, and nothing here is sourced.** They are typical adult figures, rounded to the precision I actually believe, and they exist for exactly one purpose: to choose a lattice. They are good enough for that and they are not good enough to quote.

Concretely: **before any figure on this page is written into a spec's `notes` field, check it.** This project has already shipped a fabricated citation — an orca eye patch documented as "21.8 x 5.9 cm, aspect 3.7:1, measured" in three separate places, where the numbers turned out to be two dimensionless indices lifted from an unrelated paper, and a second agent later found the identical trap in a different source. A number in a spec's notes acquires an authority it did not have in a planning document, and that is exactly how the orca figure survived review.

Where a species' presence in this biome is genuinely marginal the row says so rather than smoothing it over: `hero-sequoia` and `columnar-cypress` are not boreal trees, the European bison is a mixed-forest animal at the taiga edge, and the painted turtle reaches only the southern fringe.

The only references in this file are repo paths and line numbers, and each was verified by reading the file:

* `voxel-core/include/voxelcore/biome.h:23-42` — the `BiomeId` enum; TAIGA is id 7 at line 31.
* `voxel-core/include/voxelcore/biome.h:216-221` — the gate order: sea level, then cliff, then treeline, all before the climate table.
* `voxel-core/include/voxelcore/biome.h:225` — `if (tempU8 < kBiomeTempColdU8) return TAIGA;`, the first line of the Whittaker table and ahead of every precipitation test.
* `voxel-core/include/voxelcore/biome.h:98-99` — `kBiomeTreelineBaseMm` 900 m at 0 C (tuned, not derived) and `kBiomeTreelineMmPerDegC` 150 m per degree.
* `voxel-core/include/voxelcore/biome.h:248` — `TAIGA` returns `MAT_PODZOL`.
* `asset-forge/forge/biomes.py:71-72` — the mirrored biome row and its hosts tuple.
* `asset-forge/forge/biomes.py:37-42` — fish are hosted by the biome of the land around the water.
* `asset-forge/forge/kinds.py:29-58` — the terrain/detail lattice split and the 10 cm rule for trees and rocks.
* `asset-forge/forge/kinds.py:66-135` — the generators that exist. There is no quadruped, arthropod, chelonian, serpentine or fungus generator among them.
* `asset-forge/forge/kinds.py:85-96` — grass, reed and flower are one tuft generator with three settings.
* `asset-forge/docs/marine-megafauna-research.md:341,364-365` — the three-voxel rule and the "two to exist, three to have a shape" threshold behind it.
* `asset-forge/docs/fish-shape-research.md:53` — the eel-like body aspect ratio used in the serpentine argument.
* `asset-forge/specs/river-eel.json` — `fish.length_m` 0.70, `fish.depth_ratio` 0.085, read directly.
* `asset-forge/specs/european-robin.json` — `bird.length_m` 0.24, `bird.posture_deg` 42, `bird.neck_up_deg` 42, read directly; the basis for the above-life-size note.
* `docs/measurements/biome-screenshot-targets-2026-08-01.txt` — **6.64%** of land, best tile `-11_-6` at 83.2%, both from that file's **289-tile** column. Its earlier 121-tile pass read 6.74% with a 66.3% best tile, and the file carries two explicit corrections saying the smaller sample misled it.

Shipped spec sizes in the tables are the authored figures, not the real animal's. Where the two differ — `european-robin` and `great-tit` — that is deliberate and documented; do not "correct" a shipped size from this page.
