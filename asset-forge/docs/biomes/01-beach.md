# Beach — master species list

**Biome id 1** — `vxc::BiomeId::BEACH`, `voxel-core/include/voxelcore/biome.h:25`;
mirrored `asset-forge/forge/biomes.py:59-60`.

| | |
|---|---|
| Gate | Surface between −3 m and +4 m (`kBiomeBeachLowerMm` / `kBiomeBeachUpperMm`, `biome.h:75-76`). A **morphology** gate — `biome.h:217` returns before climate is looked at. |
| Surface material | sand (`MAT_SAND`, `biome.h:242`) |
| Share of land (shipped world) | **5.54%** over 289 tiles, and one tile (`-2_-7`) measures 100% beach (`docs/measurements/biome-screenshot-targets-2026-08-01.txt`). An earlier 121-tile pass in the same file read 5.81%; **use the 289-tile column**, which that file's own two corrections say supersedes it. |
| Water present | yes — sea, and every river mouth in the world ends here |
| Asset kinds hosted | everything: `tree`, `bush`, `rock`, `grass`, `reed`, `flower`, `fish`, `cetacean`, `bird` (`biomes.py:60`, `_FLIES`) |

Beach is a **7 m tall band wrapped around every coastline and every lake shore in
the world**, and it is decided before climate, so the same biome id covers a
tropical palm strand, a temperate shingle bank and a cold rocky shore. That is
the single most important thing about authoring for it: a beach species list has
to work in three climates at once, and the way it does that is by weighting —
a coconut palm gets a beach weight *and* a rainforest weight, a sea buckthorn
gets beach *and* grassland.

The other consequence is stated in `biome.h:218-219`: steep ground inside the
band reads BEACH rather than BARE_ROCK, because a 7 m band on a cliff is the
*foot* of the cliff. So sea stacks, wave-cut platforms and boulder shores are
beach assets, and three of them are already shipped.

## How to read the tables

**Status** is one of:

* `shipped: <spec-name>` — a spec already exists in `asset-forge/specs/`.
* `queued` — a generator exists for this kind; this is authoring work only.
* `gen: <name>` — needs a generator that does not exist. See `README.md` for the gap list.

**Lattice** (animal tables only) is the voxel size the asset should be authored
at, by the house rule in `forge/kinds.py:29-58`: a species is drawn at the
coarsest voxel size at which its smallest identifying feature is still about
three voxels across. Trees and rocks join the world's terrain grid and are 10 cm
and nothing else; ground cover, bushes, flowers and reeds are 5 cm.

**⚠** marks a species whose defining feature is at or below what the lattice will
hold, explained in the note under its table.

---

## Trees

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Coconut palm | Bare curved trunk with a ring-scarred surface, a crown of 20–30 arching pinnate fronds, nuts clustered at the crown base | 10 | `shipped: coast-palm` |
| Date palm | Straighter, stiffer, thicker trunk clad in old leaf-base stubs rather than smooth; crown held more upright | 15 | `queued` |
| Red mangrove | The prop roots are the species: a low crown standing on a tangle of arching stilts that meet the water well outside the trunk | 6 | `queued` — needs the tree generator's `roots` group pushed much further than any shipped spec uses it |
| Black mangrove | A normal low crown over a mud flat studded with thousands of finger-thick vertical pneumatophores — the ground, not the tree, is the feature | 8 | `queued` ⚠ |
| Screwpine (pandanus) | Rosettes of long strap leaves in spirals on branch tips, standing on stilt roots; reads as a palm assembled wrong | 6 | `queued` |
| Sea grape | Low spreading multi-stem tree with big round leathery leaves and drooping grape-like fruit clusters | 5 | `queued` |
| Casuarina / she-oak | Very fine drooping needle-like branchlets, dark, wind-combed to one side — the silhouette is a smudge, not a crown | 15 | `queued` |
| Monterey cypress | Heavily wind-shorn flat-topped crown with a strongly leaning trunk; the asymmetry is the species | 12 | `queued` |
| Maritime pine | Tall bare orange-barked trunk with the crown only in the top quarter | 20 | `queued` |
| Tamarisk | Feathery grey-green haze on whippy stems, often multi-stemmed from the base, pink flower plumes | 5 | `queued` |
| Beach hibiscus | Low, broad, densely leafy with round leaves and large yellow flowers that go orange-red by evening | 6 | `queued` |
| Sea buckthorn (tree form) | Silver-grey narrow leaves, dense thorny branching, dense orange berries | 4 | `queued` |
| Driftwood snag | A bleached, barkless, root-plated dead trunk lying on the sand — not a species, a state. The nearest shipped thing is `desert-dead` | 6 (fallen) | `queued` — reuse `desert-dead` |

⚠ **Black mangrove pneumatophores.** The identifying feature is a field of
1–2 cm vertical spikes over tens of metres of mud. At the terrain lattice's
10 cm, each spike is a fraction of a voxel and disappears. The honest options
are to draw them at 10 cm as a stylised forest of single-voxel columns (which is
a fiction, but a legible one) or to leave them out and build the red mangrove
instead. Do not author them at 10 cm and claim they are life-size.

## Rock types

Beach is the second-richest rock biome after bare rock, because the cliff foot
falls inside the band.

| Rock | Voxel-artist description | Block size (m) | Status |
|---|---|---|---|
| Wave-cut stack | Isolated vertical column left offshore as the cliff retreated, undercut at the waterline | 1.2 | `shipped: wave-cut-stack` |
| Sea stack (hero) | The same landform at hero scale — a tower tall enough to be a landmark | 1.2 | `shipped: hero-sea-stack` |
| Honeycomb tafoni | A face pitted with hundreds of interconnected salt-weathering cavities | 1.2 | `shipped: honeycomb-tafoni` |
| Tafoni sandstone | Larger, sparser cavernous hollows in a sandstone block | 1.2 | `shipped: tafoni-sandstone` |
| Basalt colonnade | Rank of hexagonal columns, cleanly jointed, standing vertical | 1.2 | `shipped: basalt-colonnade` |
| Basalt colonnade (hero) | The same at cliff scale | 1.2 | `shipped: hero-basalt-colonnade` |
| River cobble | Rounded, flattened, well-sorted water-worn stone | 1.2 | `shipped: river-cobble` |
| Wave-polished boulder | Rounder and smoother than a river cobble, with no facets left at all and a wet dark band at the base | 0.8 | `queued` |
| Shingle bank | Not a boulder — a graded ridge of flat pebbles all lying the same way, steep on the seaward face | 0.1 clast | `queued` |
| Rockpool platform | Flat wave-cut bench with shallow irregular pools and a step down at the seaward edge | 2.0 | `queued` |
| Tidal notch | A horizontal groove cut into a cliff foot at exactly one height, undercutting everything above it | 2.5 | `queued` |
| Sea arch | A cliff promontory pierced through at the waterline; already exists in desert form and needs a coastal palette | 1.2 | `queued` — geometry near `hero-natural-arch` |
| Chalk cliff foot | Brilliant white blocky face with horizontal flint bands and a scree of white rubble at its base | 1.5 | `queued` |
| Beachrock slab | Thin, flat, seaward-dipping cemented sand sheets breaking into rectangular plates | 1.0 | `queued` |
| Coquina / shell rock | Pale, coarse, visibly full of broken shell — a texture and palette job on an existing block | 1.0 | `queued` |
| Boulder beach | Large well-rounded blocks packed tight, all of similar size, with no sand visible between them | 0.6 | `queued` |
| Blowhole | A vertical shaft through a platform with a flared rim; the void is the asset | 2.0 | `queued` |

## Flowers

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Thrift / sea pink | Dense cushion of grassy leaves with bare stalks each holding one tight pink pompom | 0.26 | `shipped: coastal-thrift` |
| Sea holly | Rigid, spiny, entirely blue-grey — leaves and flower the same metallic colour, which is the species | 0.5 | `queued` |
| Yellow horned poppy | Grey-blue lobed leaves, large four-petal yellow flower, and a seed pod up to 30 cm long that is the identifying feature | 0.6 | `queued` |
| Sea rocket | Low fleshy sprawling mat with pale lilac four-petal flowers, growing on bare sand above the strandline | 0.3 | `queued` |
| Sea bindweed | Prostrate trailing stems over sand with large pink-and-white striped trumpets | 0.15 | `queued` |
| Beach morning glory | The tropical version: bigger fleshy round leaves, purple trumpets, long runners | 0.2 | `queued` |
| Sea lavender | Flat-topped sprays of tiny papery lilac flowers over a basal rosette, on salt marsh | 0.4 | `queued` |
| Sea aster | Fleshy narrow leaves and pale mauve daisies with a yellow centre, on tidal mud | 0.5 | `queued` |
| Sand verbena | Low sticky mat with tight round heads of pink or yellow flowers | 0.2 | `queued` |
| Sea kale | A big cabbage-like mound of thick wavy blue-grey leaves topped by a white flower cloud | 0.7 | `queued` |

## Ground cover

Includes reeds, which are their own kind (`biomes.py:35`) but the same tuft
generator.

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Meadow grass | Baseline soft tuft of fine blades | 0.45 | `shipped: meadow-grass` |
| Bulrush | Stiff vertical stems each topped with a dark brown cigar seed head | 1.4 | `shipped: bulrush` |
| Common reed | Tall vertical canes with a loose purple-brown feather plume | 2.0 | `shipped: water-reed` |
| Marram grass | Stiff, sharply in-rolled blue-green blades in scattered clumps on bare dune sand; the clump gaps *are* the dune | 0.9 | `queued` |
| Lyme grass | Wider, flatter, distinctly blue-grey blades, coarser than marram, with a stiff wheat-like head | 1.0 | `queued` |
| Sand sedge | Low, and the identifying feature is invisible: it grows in dead-straight lines across bare sand from a running rhizome | 0.25 | `queued` |
| Sea couch | Blue-green wiry grass forming continuous turf on the upper beach | 0.5 | `queued` |
| Glasswort / samphire | Leafless jointed succulent green fingers, going red in autumn — a stack of beads, not blades | 0.25 | `queued` |
| Sea purslane | Low grey-green fleshy mat along salt marsh creek edges | 0.3 | `queued` |
| Smooth cordgrass | Coarse bright green stiff blades in dense stands on tidal mud, with a one-sided seed spike | 1.2 | `queued` |
| Sea sandwort | Tight bright green fleshy rosettes creeping over bare sand in a shallow dome | 0.1 | `queued` |

## Bushes / shrubs

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Coastal scrub | Baseline wind-shorn multi-stem shrub, flat-topped on the seaward side | 0.9 | `shipped: coastal-scrub` |
| Sea buckthorn | Dense grey thorny thicket, narrow silver leaves, heavy orange berry clusters on the stems themselves | 2.5 | `queued` |
| Beach rose | Rounded dense bush, deeply wrinkled leaves, big single magenta flowers and large red hips | 1.5 | `queued` |
| Saltbush | Low grey-white mound of small mealy leaves, no thorns, very dense | 1.0 | `queued` |
| Bayberry | Upright, dark, aromatic, with tight clusters of waxy pale-blue berries on bare stem | 2.0 | `queued` |
| Tamarisk scrub | The shrub form of the tree — a feathery grey haze on whippy stems | 2.0 | `queued` |
| Mangrove sapling | A single stem with a small crown standing on a proportionally huge stilt-root cone | 1.5 | `queued` |
| Juniper (coastal prostrate) | The shipped juniper laid flat and spread wide by wind rather than grown upright | 1.1 | `shipped: juniper-scrub` — needs a beach weight |
| Gorse | Dense spiny green mound covered in coconut-scented yellow pea flowers; the spines are the leaves | 1.8 | `queued` |

## Birds

Beach is the shore-and-wader biome, and it shares its gulls with ocean and its
songbirds with grassland. All authored at 1 cm.

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Herring gull | White body, grey mantle, black wingtips, yellow bill with a red spot | 0.60 | 1 cm | `shipped: herring-gull` |
| Grey heron | Very long neck folded into an S, dagger bill, grey with a black crest plume | 1.00 | 1 cm | `shipped: grey-heron` |
| Mallard | Heavy-bodied duck, bottle-green head and white collar on the drake | 0.58 | 1 cm | `shipped: mallard-duck` |
| Common kestrel | Small falcon, pointed wings, long barred tail, rufous back | 0.34 | 1 cm | `shipped: common-kestrel` |
| Common starling | Compact, short-tailed, triangular-winged, glossy black spangled with pale spots | 0.21 | 1 cm | `shipped: common-starling` |
| Barn swallow | Deeply forked tail streamers, blue-black above, red throat. **Authored at 26 cm against a real 17–19 cm** | 0.26 | 1 cm | `shipped: barn-swallow` |
| Rock pigeon | Stocky grey body, two black wing bars, iridescent neck | 0.33 | 1 cm | `shipped: rock-pigeon` |
| Eurasian oystercatcher | Hard black-and-white body with a long straight bright orange bill and pink legs; the loudest silhouette on any shore | 0.42 | 1 cm | `queued` |
| Sanderling | Tiny, pale, almost white, running at the water's edge; short straight black bill | 0.20 | 1 cm | `queued` ⚠ authored-up |
| Ringed plover | Round-bodied small wader, one black breast band and a black-and-white face mask | 0.19 | 1 cm | `queued` ⚠ authored-up |
| Eurasian curlew | The bill is the species: long, thin, evenly downcurved, a third of the total length | 0.55 | 1 cm | `queued` |
| Bar-tailed godwit | Long straight-to-slightly-upturned bill, long legs, rusty in summer | 0.38 | 1 cm | `queued` |
| Pied avocet | Slim white bird with a black cap and wing panels and a fine strongly **up**curved bill | 0.44 | 1 cm | `queued` |
| Ruddy turnstone | Squat, short-legged, harlequin-patterned tortoiseshell back with orange legs | 0.23 | 1 cm | `queued` |
| Common redshank | Mid-sized brown wader with bright orange-red legs and bill base | 0.28 | 1 cm | `queued` |
| Common tern | Slim, long forked tail, black cap, orange-red bill with a black tip | 0.34 | 1 cm | `queued` |
| Sandwich tern | Larger, shaggier crest, black bill with a **yellow tip** — the tip is the only separator from the common tern | 0.40 | 1 cm | `queued` |
| Black-headed gull | Small gull with a chocolate (not black) hood in summer and a white leading edge to the wing | 0.38 | 1 cm | `queued` |
| Little egret | Pure white, black legs with **yellow feet**, black dagger bill, two long nape plumes | 0.60 | 1 cm | `queued` |
| Great cormorant | Long low body, snake neck, hooked bill, held wings-open to dry | 0.90 | 1 cm | `queued` |
| Osprey | White below, dark above, a dark eye stripe, and wings held with a kink at the wrist | 0.60 | 1 cm | `queued` |
| Brown pelican | Enormous pouched bill, heavy body, short neck | 1.20 | 1 cm | `queued` |
| Snowy plover | Very pale small plover on dry sand, partial dark neck patches rather than a full band | 0.16 | 1 cm | `queued` ⚠ authored-up |

⚠ **Sanderling, ringed plover, snowy plover.** All 16–20 cm. The library already
has four birds authored above life size to clear a 20 cm floor, and the bird
research recorded a further reason: a perched bird is authored at 36–42° nose-up
and 20 cm of bird at 42° projects onto sixteen voxels of *length*. Small waders
stand nearly horizontal so they lose less to that, but they are still at the
floor. Author them at 22–24 cm and write the reason in `notes`.

## Land animals

The beach hosts land animals the moment a quadruped generator exists — it is a
plantable biome with a full `_FLIES` host tuple, so nothing engine-side blocks it.

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| Grey seal | Torpedo body hauled out on rock, long roman muzzle, blotched grey, hind flippers fanned | 2.3 | 5 cm | `gen: pinniped` |
| Harbour seal | Shorter, rounder, concave dog-like face; hauls out on sandbars | 1.7 | 5 cm | `gen: pinniped` |
| Californian sea lion | Longer neck, external ear flaps, walks on its foreflippers | 2.2 | 5 cm | `gen: pinniped` |
| Green sea turtle (nesting) | Flat oval shell, paddle flippers, dragging a body track up the sand | 1.1 shell | 2 cm | `gen: chelonian` |
| Loggerhead turtle | Larger blunt block head, reddish-brown shell with a raised central ridge | 1.0 shell | 2 cm | `gen: chelonian` |
| Marine iguana | Flattened tail longer than the body, blunt square snout, a low spiny dorsal crest, black or rust-red, lying flat on black rock | 0.6 (1.2 with tail) | 2 cm | `gen: quadruped` |
| Water monitor | Long low sprawling lizard, forked tongue, banded flanks, tail a good half of total length | 1.5 (2.5 total) | 2 cm | `gen: quadruped` |
| Red fox | Slender dog silhouette, black-backed pointed ears, white-tipped brush tail as thick as the body | 0.7 / 0.4 sh | 2 cm | `gen: quadruped` |
| Raccoon | Low hunched body, black eye mask, ringed tail; the mask and the rings are the entire species | 0.5 / 0.3 sh | 2 cm | `gen: quadruped` ⚠ |
| European otter | Long low sinuous body, thick tapering tail, short legs, flat broad muzzle | 0.8 (1.2 with tail) | 2 cm | `gen: quadruped` |
| Wild boar | Wedge-shaped body heaviest at the shoulder, dropped head, short legs, coarse dark bristle | 1.4 / 0.8 sh | 5 cm | `gen: quadruped` |
| Eastern grey kangaroo | Sits on a heavy tail as a third leg, tiny forelimbs, huge hindquarters, upright neck; genuinely a beach animal on parts of the Australian coast | 1.3 / 1.5 tall | 5 cm | `gen: quadruped` |
| Coati | Long banded tail carried straight up, long flexible upturned snout | 0.6 (1.2 with tail) | 2 cm | `gen: quadruped` |
| Small Indian mongoose | Very low, very long, short-legged, uniform grizzled brown, tapering tail | 0.35 (0.65 total) | 1 cm | `gen: quadruped` ⚠ |
| Dingo | Lean sandy-ginger dog with erect ears and a straight bushy tail carried low | 0.9 / 0.55 sh | 2 cm | `gen: quadruped` |
| Ghost crab | Pale sand-coloured square carapace, two stalked eyes standing well clear, one claw larger | 0.05 across | 1 cm | `gen: arthropod` ⚠ |
| Coconut crab | Very large land crab, heavy blue or orange, one massive claw, legs spanning up to a metre | 0.4 body / 1.0 span | 2 cm | `gen: arthropod` |
| Sand hopper | A few millimetres. Never an asset at any lattice this project has; it belongs in a particle effect if it belongs anywhere | 0.02 | — | out of scope |

⚠ **Three lattice notes.**

* **Raccoon.** The tail rings are 3–4 cm on a 30 cm tail. At 5 cm one ring is one
  voxel and the tail reads as a smear; at 2 cm a ring is two voxels, which is the
  documented floor and no more. The rings, not the raccoon, choose the lattice.
* **Mongoose.** 35 cm head-body at 1 cm is 35 voxels, which is fine — but it has
  no marking at all, so the *only* things carrying identity are the length:height
  ratio and the taper of the tail. That is a shape-only species, and shape-only
  species are the ones that need the most care at small sizes.
* **Ghost crab.** 5 cm across is five voxels at 1 cm, and its two identifying
  features — the stalked eyes and the asymmetric claw — are each 1 cm. This is
  below the floor. Author it at 8–10 cm, or accept that it reads as a pale lump
  and only its motion identifies it.

## Fish

Beach fish are the shallow, brackish and surf-zone species, plus the
inshore-ranging sharks and dolphins that already exist. The engine tags a river
mouth's fish by the *land* around it (`biomes.py:37-42`), so estuarine species
sit here.

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Clown anemonefish | Orange, three white bars. **Authored at 22 cm against a real ~10 cm** | 0.22 | 1 cm | `shipped: clown-anemonefish` |
| Reef tang | Disc body, tiny mouth, one continuous dorsal | 0.22 | 1 cm | `shipped: reef-tang` |
| Pale minnow | Small silver fusiform, no marking, countershaded | 0.20 | 1 cm | `shipped: pale-minnow` |
| Shoal herring | Slim silver, deeply forked tail | 0.20 | 1 cm | `shipped: shoal-herring` |
| River eel | Heavy anguilliform, continuous fin ridge, olive-brown | 0.70 | 1 cm | `shipped: river-eel` |
| Reef shark | Compact grey requiem shark with marked fin tips | 1.60 | 2 cm | `shipped: reef-shark` |
| Great white shark | Conical snout, hard countershade line | 4.50 | 5 cm | `shipped: great-white-shark` |
| Tiger shark | Blunt square snout, faint bars | 4.00 | 5 cm | `shipped: tiger-shark` |
| Scalloped hammerhead | Scalloped hammer head with eyes at the tips | 3.50 | 5 cm | `shipped: scalloped-hammerhead` |
| Bottlenose dolphin | Grey, short beak, tall falcate dorsal | 3.00 | 5 cm | `shipped: bottlenose-dolphin` |
| Common dolphin | Yellow-buff hourglass on the flank | 2.40 | 2 cm | `shipped: common-dolphin` |
| Orca | Black and white with an eye patch and a grey saddle | 7.00 | 5 cm | `shipped: orca` |
| European flounder | Flat oval, both eyes on one side, brown with paler blotches, lying on sand | 0.35 | 1 cm | `queued` |
| Common sole | Narrow oval flatfish with a small mouth set at one end, uniform sandy brown | 0.35 | 1 cm | `queued` |
| Thick-lipped grey mullet | Blunt-headed, heavy-lipped, two well-separated dorsals, grey with faint horizontal stripes; the species that noses about in harbours | 0.55 | 2 cm | `queued` |
| European sea bass | Clean silver fusiform, spiny first dorsal | 0.70 | 2 cm | `queued` |
| Garfish | Extremely elongated with both jaws drawn out into a long thin beak, green bones, silver flank | 0.75 | 1 cm | `queued` ⚠ |
| Lesser sand eel | A silver thread, essentially depth-free, that shoals in thousands and buries in sand | 0.20 | 1 cm | `queued` ⚠ |
| Small-spotted catshark | Slim sandy shark with dense small dark spots, resting on the bottom in a curve | 0.75 | 2 cm | `queued` |
| Thornback ray | Flat diamond with a row of heavy thorns down the back and tail, mottled brown | 0.7 span | 2 cm | `queued` |
| Shanny / common blenny | Blunt-headed, big-eyed, single long dorsal running the whole back, mottled; a rockpool species | 0.15 | 1 cm | `queued` ⚠ authored-up |
| Sand goby | Tiny translucent sandy fish with two dorsals and a rounded tail; near-invisible by design | 0.09 | 1 cm | `queued` ⚠ |
| Atlantic mudskipper | The one fish that walks: props itself on muscular pectoral fins, eyes on top of the head, out of the water on mud | 0.20 | 1 cm | `queued` |
| Grey shrimp | Not a fish. Listed because a beach without it is missing something, and because it is `gen: arthropod` and would go in a shoal | 0.07 | 1 cm | `gen: arthropod` ⚠ |

⚠ **Four lattice notes.**

* **Garfish.** A near-15:1 body with a beak that is a fifth of total length and
  perhaps 8 mm thick. At 1 cm the beak is one voxel wide along its whole run,
  which is the absolute floor — draw it two voxels thick and accept that it is
  stylised.
* **Sand eel.** The fish research already measured this case: an eel-class body
  at 25 voxels has a caudal peduncle of 0.2 voxels, and any code assuming a
  cross-section always contains a cell centre produces the fish in three pieces.
  A sand eel is that case at its most extreme. It will only work because the body
  axis is stamped as a solid one-voxel run first.
* **Sand goby, shanny, shrimp.** All 7–15 cm, all under the 20-voxel floor at
  1 cm. Author up or leave out. The honest recommendation is to build the shanny
  (author at 20 cm) and skip the goby, because a deliberately cryptic 9-voxel
  fish is a lot of work to produce something the player is designed not to see.
* **Flatfish generally.** A depressiform body is wider than it is deep, which is
  the one cross-section family the fish generator has never been authored for.
  Build one and look at it before committing to four.

---

## Build priority

Beach is the highest-value biome per asset in the world for one reason that has
nothing to do with its 5.54% share: **it is the only biome that wraps every
coastline and every lake shore**, so a player walking any water's edge anywhere
sees beach assets. It is also where the sea meets the land, which is where a
world looks most obviously empty when it is.

1. **Dune grasses.** Marram, lyme grass, sea couch, sand sedge. Four tuft specs,
   the generator is shipped and proven, and bare sand with nothing on it is the
   most conspicuous emptiness in the biome. Cheapest win available.
2. **Waders.** Oystercatcher, curlew, avocet, turnstone, ringed plover. The bird
   generator is shipped, they are all 1 cm, and their identity lives almost
   entirely in **bill shape** — which is exactly the axis `bird.bill_frac`,
   `bill_curve` and `bill_depth` already parameterise. Five species for very
   little new geometry.
3. **The coastal rocks.** Rockpool platform, shingle bank, boulder beach, chalk
   cliff foot. The rock generator is the most-exercised in the library (34 specs)
   and these are parameter work.
4. **Two or three shore trees** — date palm, casuarina, tamarisk — to break the
   monotony of a single palm on every tropical shore.
5. **Shallow fish.** Flounder, mullet, sea bass, catshark. Flounder first, as the
   depressiform probe.
6. **Seals, last of the animals but first when the quadruped work lands.** A
   pinniped is the *easiest* possible first test of a new animal generator: no
   legs to place, no gait, a single fused hind flipper, and a resting pose that
   sits flat on the ground. If a quadruped generator is being written, a seal is
   the shape to debug it on before anything with four legs.

## Where the numbers come from

Every size in this file is an **approximate typical adult figure from general
knowledge**. None of it is measured, and none of it is sourced. It is good
enough to choose a voxel lattice, which is all this document is for, and it is
**not** good enough to quote as a fact or to paste into a spec's `notes` without
checking first.

The repo references — `biome.h` and `biomes.py` line numbers, the land share from
`docs/measurements/biome-screenshot-targets-2026-08-01.txt`, shipped spec names
and their authored sizes — were read out of the files and are exact. **The land
share is quoted from that file's 289-tile column, not its 121-tile one**; the
file carries two explicit corrections saying the smaller sample misled it, and
the biome shares moved by up to 6.7 points between the two.

Specific hedges:

* **The Lattice column is a recommendation from the three-voxel rule, not a
  measurement.** Only the shipped rows have a lattice that has been tested. Every
  `queued` row should be confirmed with `tools/fishprobe.py --lattice` or the bird
  equivalent before the spec is written.
* **Two range calls I am not confident in.** The eastern grey kangaroo on beaches
  and the small Indian mongoose are both real but geographically narrow, and the
  mongoose is introduced almost everywhere it is now common. If the world is not
  meant to model introductions, drop it.
* **The pneumatophore, ghost crab and sand goby entries are arguments for leaving
  something out**, not specs. They are in the list so nobody re-derives the same
  problem in three months.
