# Rainforest — master species list

**Biome id 4** — `vxc::BiomeId::RAINFOREST`, `voxel-core/include/voxelcore/biome.h:28`; mirrored `asset-forge/forge/biomes.py:65-66`.

| | |
|---|---|
| Climate envelope | precipitation >= 1600 mm/yr AND mean annual temperature >= 18 C (`biome.h:234`, thresholds at `biome.h:137` and `biome.h:121`) |
| Surface material | jungle soil — `MAT_JUNGLE_SOIL`, `biome.h:245` |
| Share of land (shipped world) | **4.73%** over 289 tiles, best tile `5_-14` at 89.8% (`docs/measurements/biome-screenshot-targets-2026-08-01.txt`). An earlier 121-tile pass in the same file read 4.08% with a best tile of 59.5%; **use the 289-tile column**, which that file's own two corrections say supersedes it. |
| Water present | yes — rivers and lakes run through it; the engine classifies a river by the land around it (`biomes.py:37-42`), so a river fish here is tagged rainforest |
| Asset kinds hosted | tree, bush, rock, grass, reed, flower, fish, cetacean, bird (`biomes.py:65-66` via `_FLIES`, `biomes.py:50`) |

This is the smallest of the vegetated biomes. **A claim that was in this file's first draft has been withdrawn**: on the 121-tile census no tile was more than 59.5% rainforest, and it looked as though the biome was always seen mixed into its neighbours. On 289 tiles the best tile is 89.8%, and the census file's own note records that every biome now has a dominant tile. So rainforest *can* fill a frame, and the asset list has to hold up as a country of its own as well as a wet dark band along a river. What that buys the asset list is that the biome is read at close range, in the understory, from inside: the emergent canopy is a ceiling and the things the player actually stands next to are trunk bases, buttress roots, big leaves and the water. It also means the biome is layered — canopy, understory, floor, water — and an asset list that only fills the canopy will look empty at head height.

## How to read the tables

**Status** is one of:
* `shipped: <spec-name>` — a spec already exists in `asset-forge/specs/`.
* `queued` — a generator exists for this kind; this is authoring work only.
* `gen: <name>` — needs a generator that does not exist. See the index for the gap list.

**Lattice** (animal tables only) is the voxel size the asset should be authored at, by the house rule in `asset-forge/forge/kinds.py:29-58`: a species is drawn at the COARSEST voxel size at which its smallest identifying feature is still about three voxels across. Trees and rocks are not listed with a lattice because they join the world's terrain grid and are 10 cm and nothing else; ground cover, bushes, flowers and reeds are 5 cm.

**⚠** marks a species whose defining feature is at or below what the lattice will hold, explained in the note under its table.

## Trees

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Jungle emergent | Bare column for two thirds of its height, then a wide flat crown spread above everything else; buttress roots flaring out at the base. | 28 | `shipped: jungle-emergent` |
| River broadleaf | Leaning out over the channel with an asymmetric crown weighted to the water side, dense dark foliage down to about half height. | 12 | `shipped: river-broadleaf` |
| Coast palm | Bare curved stem with a ring-scarred grey trunk and a single crown of long arching fronds; no branches at all. | 10 | `shipped: coast-palm` |
| Kapok (ceiba) | The biggest silhouette here: pale grey almost white trunk, thin plank buttresses standing out 2-3 m from the base like walls, horizontal branch tiers and a flat umbrella crown far above the canopy. | 45 | `queued` |
| Strangler fig | A hollow basket of fused vertical roots where a trunk should be, ragged holes through it, topped with a dense dark rounded crown. | 25 | `queued` |
| Cecropia | Candelabra of a few thick straight branches from a pale ringed trunk, very few leaves, each one a big round palmate plate with a silver-white underside. | 15 | `queued` |
| Wild banana / plantain | No woody trunk — a fat green pseudostem, and a crown of four to eight enormous paddle leaves torn into strips along their ribs. | 5 | `queued` |
| Tree fern | Slender fibrous dark trunk, no taper, capped with a flat rosette of finely divided fronds; the trunk is a third the width of a palm's. | 6 | `queued` |
| Giant bamboo clump | A tight sheaf of 20-40 straight jointed poles from one base, leaning apart at the top, pale yellow-green with dark node rings every 30-40 cm. | 18 | `queued` |
| Red mangrove | Low spreading crown carried on a tangle of arching stilt roots that meet the mud well outside the trunk; the roots are half the silhouette. | 8 | `queued` |

Range note: mangrove is the marginal one. It lives at the tidal edge, and ground within about ±3 m of sea level is decided as beach before the climate table is reached — so it may only ever place on the landward side of an estuary, if at all. Worth authoring, worth checking placement before it is scheduled.

Generator note: the tree generator's three growth models (`kinds.py:66-71`) cover most of this table — colonize for the broadleaf crowns, whorl for the cecropia candelabra and the bamboo sheaf, frond for the palm, banana and tree fern. The strangler fig's hollow root basket is the one shape here that is not a trunk, and it may come out as a very heavy `roots` setting rather than a new model; that is worth trying before anything is built for it.

## Rock types

| Rock | Voxel-artist description | Block size (m) | Status |
|---|---|---|---|
| Tsingy pinnacles | Limestone blades on a shared plinth, gaps wide at the top and closed at the base, flanks fluted by running water. | 3.0 | `shipped: hero-tsingy-pinnacles` |
| Mossy forest boulder | Rounded part-buried block, dark wet grey where bare, a thick green moss cap over the whole upper surface and ferns rooted in its seams. | 1.2 | `shipped: mossy-forest-boulder` |
| Laterite crust block | Blocky rust-red slab with a hard pitted crust, riddled with rounded holes a few cm across, crumbling to ochre at broken edges. | 1.0 | `queued` |
| Rapids whaleback | Smooth grey rounded dome standing in the channel, polished and undercut in a horizontal band at the low-water line, black algae above it. | 2.5 | `queued` |
| Karst breakdown block | Angular pale limestone slab fallen from a collapsed cave roof, one clean flat face and three broken ones, sharp edges softened by moss only on top. | 2.0 | `queued` |
| Quartz-vein cobble bar | A low spread of rounded river cobbles, mixed dark grey and white-veined, sized to sit in a bar rather than stand alone. | 0.35 | `queued` |

## Flowers

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Jungle understory flower | Few broad dark leaves low down, one or two pale blooms held clear on thin stems. | 0.75 | `shipped: jungle-understory-flower` |
| Heliconia (lobster claw) | A fan of paddle leaves and one hanging zigzag stem of stacked waxy bracts, scarlet with yellow-green tips — the bracts, not the flower, are the mark. | 1.6 | `queued` |
| Wild ginger | Broad ribbed leaves on cane-like stems, with a tight cone of red or pink bracts held near the ground beneath them. | 1.2 | `queued` |
| Impatiens | Low untidy mound of soft translucent stems with many small flat five-petal blooms in pink and white scattered across the top. | 0.35 | `queued` |
| Terrestrial bromeliad | A tight rosette of stiff strap leaves arcing out from a central cup, leaf tips arching back down, a single red-orange spike from the middle. | 0.5 | `queued` |

Placement note: most rainforest bromeliads and all its orchids are epiphytes — they live on branches, not on the ground. Placement puts flowers on the ground, so the row above is deliberately the terrestrial kind. Getting the epiphyte look would need assets that attach to a tree, which nothing in `kinds.py` does; that is a placement gap, not an authoring one, and it is not on the generator gap list.

## Ground cover

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Jungle groundcover | Broad soft blades arcing out from a crown, darker and wider than any temperate grass, sparse enough to see soil between tufts. | 0.8 | `shipped: jungle-groundcover` |
| Water reed | Tall near-vertical stems in a stand at the water's edge, most carrying a seed head. | 2.0 | `shipped: water-reed` |
| Papyrus | Bare triangular stems standing 3 m out of the shallows, each topped with a spherical burst of thin green rays — a stem with a firework on it. | 3.5 | `queued` |
| Understory fern | A low shuttlecock of arching fronds, each frond a flat plane cut into many small leaflets, mid-green above and pale beneath. | 0.9 | `queued` |
| Spikemoss mat ⚠ | A flat continuous carpet of tiny overlapping scale-leaves running over soil and rock, blue-green with an iridescent sheen in shade. | 0.08 | `queued` |

⚠ Spikemoss: at the 5 cm ground-cover lattice the whole plant is one or two voxels tall and an individual frond is sub-voxel. It cannot be an individual plant. Either author it as a mat — one wide low slab with colour variation and no resolved leaves — or accept that it is a ground material and not an asset at all, and put the look in the surface texture instead.

## Bushes / shrubs

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Coffee shrub | Upright multi-stem shrub with flat horizontal branch tiers, glossy dark oval leaves in opposite pairs, small red berries clustered against the stems. | 2.5 | `queued` |
| Understory fan palm | Short stubby or absent trunk carrying three to six stiff pleated fans, each fan a near-flat disc split into wedges. | 3.0 | `queued` |
| Wild hibiscus | Loose open shrub with sparse toothed leaves and a few large flat five-petal blooms in red or yellow, each with a long protruding central column. | 2.5 | `queued` |
| Dracaena thicket | Several bare canes leaning apart from one base, each topped with a tuft of long strap leaves — a shrub built like a small palm. | 2.0 | `queued` |
| Monstera clump | Sprawling clump of thick stems carrying very large dark heart-shaped leaves, each cut with holes and deep slashes to the midrib. | 1.5 | `queued` |

## Birds

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Scarlet macaw | Long tapered tail more than half the total length, huge deep hooked pale bill, crimson body with a blue wing, a yellow wing panel and a bare white face. | 0.85 | 1 cm | `shipped: scarlet-macaw` |
| Grey heron | Tall, long-necked, kinked neck folded back in rest, dagger bill, grey above and white below with a black eye-stripe trailing into a crest. | 1.0 | 1 cm | `shipped: grey-heron` |
| Common kingfisher | A bill and a head with a bird behind it: straight dagger bill a fifth of total length, almost no tail, electric blue back over an orange breast. Authored at 20 cm against a real 17 — see the spec's own notes. | 0.20 | 1 cm | `shipped: common-kingfisher` |
| Toco toucan | Body of a crow carrying an enormous curved orange bill nearly a third of total length; black body, white bib, blue eye-ring. | 0.6 | 1 cm | `queued` |
| Great hornbill | Heavy body, long broad bill with a hollow block of a casque sitting along its top, black-and-white wings with wide white bars, long banded tail. | 1.1 | 1 cm | `queued` |
| Harpy eagle | Massive-chested raptor with short broad wings, a double crest that splits into two horns, grey head, black chest band, white belly and very heavy legs. | 1.0 | 1 cm | `queued` |
| Great blue turaco | Long-tailed, thickset, standing crest of blunt black feathers over the forehead, blue-green body with a yellow-and-red bill and a broad black tail band. | 0.75 | 1 cm | `queued` |
| Hoatzin | Small body, huge loose ragged crest of spiky feathers, bare blue face with a red eye, heavy chestnut-and-cream wings, long broad tail. | 0.65 | 1 cm | `queued` |
| Sunbittern | Slim, heron-like on short legs, cryptic barred grey-brown at rest; the mark is on the spread wing — a large chestnut-and-black eyespot on each. | 0.5 | 1 cm | `queued` |
| Great curassow | Ground bird the size of a turkey, glossy black with a white belly, a tight forward-curling crest of coiled feathers and a yellow knob on the bill. | 0.9 | 1 cm | `queued` |
| Wattled jacana ⚠ | Slender rail-like waterbird, chestnut body with black head, yellow bill shield, and absurdly long thin toes spreading it across floating leaves. | 0.25 | 1 cm | `queued` |
| Hummingbird ⚠ | Tiny, near-tailless, needle bill as long as the body, iridescent green back with a saturated throat patch; drawn hovering rather than perched. | 0.11 | 1 cm | `queued` |

⚠ Jacana: the toes are the species. They are around 1 cm thick and 6-7 cm long, so at the 1 cm bird lattice each toe is one voxel wide — one voxel reads as a mistake, not a feature. The honest fix is the one the library already used for the kingfisher: draw the feet above life size, take the width to about 3 cm, and write that decision into the spec's notes.

⚠ Hummingbird: at 11 cm on a 1 cm lattice the whole bird is 11 voxels long, and the bird generator is described as drawing to read at 20-90 voxels (`kinds.py:129-134`). It is below the range the generator was built for. Either author it oversized at around 20 cm, or drop it — a hummingbird at true scale will be a smudge whatever the lattice.

## Land animals

Nothing in this table is shipped, and nothing in it can be built today: there is no quadruped generator anywhere in `kinds.py:66-135`. This is the largest gap in the project.

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| Western lowland gorilla | Huge sloping forequarter, long arms reaching the ground and short legs, conical crested head sunk into the shoulders, matte black with a pale grey saddle on adult males. | 1.7 standing / 0.85 at the shoulder on all fours | 2 cm | `gen: quadruped` |
| Chimpanzee | Lighter build than a gorilla, flat black coat, arms longer than legs, pale bare face with big ears standing clear of the head. | 1.3 standing / 0.7 on all fours | 2 cm | `gen: quadruped` |
| Mandrill | Heavy dog-like muzzle on a stocky grey-brown body, short upright tail; the mark is the face — a red stripe down the middle with deep blue ridged flanges either side. | 0.8 head-body / 0.5 at the shoulder | 1 cm | `gen: quadruped` |
| Black howler monkey ⚠ | Compact black body, deep swollen throat and jaw giving a square heavy head, and a long prehensile tail carried in a curl, usually longer than the body. | 0.6 head-body, 0.6 tail | 1 cm | `gen: quadruped` |
| Bengal tiger ⚠ | Long low heavy-shouldered cat, deep chest, thick tail two thirds of body length, orange with a white belly and narrow black vertical stripes. | 2.0 head-body / 0.95 at the shoulder | 2 cm | `gen: quadruped` |
| Jaguar ⚠ | Stockier and shorter-legged than any other big cat, very broad blocky head, tawny with dark rosettes that each enclose one or two small spots. | 1.7 head-body / 0.75 at the shoulder | 2 cm | `gen: quadruped` |
| Okapi ⚠ | Dark red-brown body with a giraffe's sloping back and long neck at half scale, and bold white horizontal stripes on the rump and upper legs only. | 2.1 head-body / 1.5 at the shoulder | 2 cm | `gen: quadruped` |
| Bongo | Deep chestnut forest antelope with a heavy body and short legs, 10-14 thin vertical white stripes down the flanks, and open spiralled horns swept back. | 2.2 head-body / 1.2 at the shoulder | 2 cm | `gen: quadruped` |
| Red river hog | Low slab-sided pig on short legs, bright rust-red back with a white crest along the spine, long white tufts on the ear tips and a white eye-ring. | 1.3 head-body / 0.7 at the shoulder | 2 cm | `gen: quadruped` |
| Forest buffalo | Small dark red-brown buffalo, heavy square head carried low, short horns that sweep back rather than out, and long fringed ears. | 2.2 head-body / 1.1 at the shoulder | 5 cm | `gen: quadruped` |
| African forest elephant | Smaller and rounder-backed than the savanna elephant, straight downward-pointing tusks, oval ears held flat, dark wet-looking hide. | 3.0 head-body / 2.4 at the shoulder | 5 cm | `gen: quadruped` |
| Lowland tapir | Barrel body tapering to a narrow front end, arched back, short mobile trunk, stiff bristle crest along the neck, uniform dark grey-brown. | 2.0 head-body / 1.0 at the shoulder | 5 cm | `gen: quadruped` |
| Capybara | A blunt brick of a rodent: rectangular body, square blocky head with a flat muzzle, no visible tail, coarse red-brown hair, short legs. | 1.1 head-body / 0.55 at the shoulder | 2 cm | `gen: quadruped` |
| Giant anteater | Long tubular head with no visible eye or ear at range, low body, and an enormous flag of a tail nearly as long again; grey with a black wedge edged white across the shoulder. | 1.2 head-body, 0.8 tail / 0.6 at the shoulder | 2 cm | `gen: quadruped` |
| Brown-throated sloth | A hanging shape, not a standing one: rounded body, very long hooked forelimbs, flat round face, shaggy grey-green coat. | 0.6 head-body | 2 cm | `gen: quadruped` |
| Green anaconda | Huge-girthed olive-green snake with round black blotches down the back and two dark stripes behind the eye, thickest at mid-body and abruptly tapered. | 5.0 long | 5 cm | `gen: serpentine` |
| Water monitor lizard | Sprawling four-legged lizard with the legs out sideways, low belly, long deep-based tail more than half the total, dark grey with pale yellow spot rows. | 1.8 total, 0.75 head-body | 2 cm | `gen: quadruped` |
| Spectacled caiman | Flat-bodied and wide, sprawling stance, long broad snout with a bony ridge between the eyes, raised paired scutes running the length of the tail. | 2.0 total | 2 cm | `gen: quadruped` |
| Yellow-footed tortoise | Domed oval shell with raised segment plates, dark brown with a yellow-orange patch at the centre of each plate, and orange-blotched scaly legs. | 0.4 shell | 1 cm | `gen: chelonian` |
| Poison dart frog | Squat, wide-mouthed, long folded hind legs, held nose-up; the whole point is flat saturated colour — one bright ground with two or three black bands or blotches. | 0.045 | 1 cm | `gen: quadruped` |
| Goliath birdeater tarantula ⚠ | Low wide body in two segments, eight thick hairy legs held in a flat radial spread twice the body's width, uniform dark red-brown. | 0.28 legspan | 1 cm | `gen: arthropod` |

⚠ Howler monkey: the tail is most of the silhouette and it is around 3 cm thick. At 2 cm it is one to two voxels and breaks up; 1 cm gives three and holds, which is why this animal is on the same lattice as a bird rather than the same one as a cat.

⚠ Tiger: the stripes are roughly 4-8 cm wide on a 2 m body. At 5 cm each stripe is one voxel and the pattern collapses into noise; at 2 cm it is two to four voxels and reads. The stripe chooses the lattice, not the animal.

⚠ Jaguar: rosettes are the same order of size as a tiger's stripes with the extra problem that a rosette is a ring, so it needs an outside, a wall and an inside — three features across, not one. 2 cm is the coarsest that can hold it and it will still be a soft ring rather than a crisp one.

⚠ Okapi: the rump stripes are about 5 cm wide with 5 cm gaps. 2 cm gives roughly two-and-a-half voxels per bar, which is marginal. If it reads badly, widen the bars rather than going finer — the animal is otherwise plain enough to sit happily at 5 cm.

⚠ Tarantula: leg thickness is about 1 cm, so at the 1 cm lattice a leg is one voxel and eight of them will read as a smear rather than eight legs. Author the legs at 2-3 cm thick, above life size, and record it in the spec notes. Note also that this sets a floor for the whole biome: leafcutter ants, most beetles and any frog under about 4 cm are below what a voxel asset can represent at all, and belong in a particle or decal system rather than in this list.

A note on `gen: serpentine`: this is probably the least work of the four missing generators, because it is probably not a new generator at all. The fish generator already lofts `river-eel` at a depth ratio of 0.085 — a body about seventeen times longer than it is deep — with its dorsal and anal fins collapsed to a continuous low ridge. Turn the fins off entirely, take the tail to a point, and an anaconda is closer to shipped than any mammal in this table.

## Fish

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Mud catfish | Wider than deep, flat-bottomed, big flat head a third of the length, four barbels off the snout, mottled brown. | 0.38 | 1 cm | `shipped: mud-catfish` |
| River eel | Almost no depth, one continuous fin ridge instead of separate dorsal and anal fins, snout and wrist nearly as deep as the middle, olive-black over yellow. | 0.70 | 1 cm | `shipped: river-eel` |
| Arapaima | Enormous cylinder with the dorsal and anal fins set far back near the tail, flat armoured head, dark olive forward, shading to a broad red-scaled wash over the back third. | 2.5 | 2 cm | `queued` |
| Red-bellied piranha | Deep, laterally flat disc of a body, blunt steep forehead, undershot jaw, silver flanks with fine dark speckling and a solid orange-red throat and belly. | 0.28 | 1 cm | `queued` |
| African tigerfish | Streamlined and deep-shouldered, forked tail, silver with thin dark horizontal stripes along the flanks, red-orange fins, and visible interlocking teeth. | 0.7 | 1 cm | `queued` |
| Electric eel | Long cylinder with a flat head and a single very long anal fin running most of the underside; no dorsal fin at all, dark grey-brown with a yellow-orange throat. | 2.0 | 2 cm | `queued` |
| Peacock cichlid | Deep oval body, blunt head, long low dorsal running most of the back, gold-green flanks with three dark vertical bars and a black eyespot on the tail base. | 0.5 | 1 cm | `queued` |
| Armoured catfish | Flat-bottomed and plated, sucker mouth under a broad head, tall fan of a dorsal fin held up like a sail, dark mottled grey-brown all over. | 0.3 | 1 cm | `queued` |
| Freshwater stingray | A flat disc with eyes on top and a thin whip tail as long again; dark brown with a scatter of pale ringed spots across the disc. | 0.9 disc, 1.6 total | 2 cm | `queued` |
| Amazon river dolphin (boto) | Long slender beak with visible bristles, a bulging melon that changes shape, no real dorsal fin — a low ridge instead — very broad paddle flippers, and pink over grey. | 2.3 | 5 cm | `queued` |

The boto is a `cetacean`, not a `fish`, and the cetacean generator exists (`kinds.py:119-123`). It is the only large animal in this biome that can be built today with no new code, which is why it is worth more than its share of attention.

The stingray is the one row here that strains the generator. `fish` is a single solid whose cross-section changes along one axis (`kinds.py:101-106`); a disc that is much wider than it is long, with a whip behind it, is at the edge of what that can loft. It may come out fine and it may come out as a flat fish rather than a ray — try it before deciding it needs anything new.

## Build priority

1. **The land-animal generator.** Twenty-one rows in this file are blocked on one missing thing, and no biome anywhere in the library has a single land animal. Nothing else on this list changes the biome as much as the first quadruped will.
2. **`gen: serpentine`, as a fish variant.** Cheapest real animal in the file, for the reason given above — an anaconda is an eel with the fins turned off, and the eel is already shipped.
3. **Understory plants: fern, monstera clump, heliconia, wild ginger.** Authoring only, generators all exist, and they fix the specific thing that is wrong with the biome at head height — the shipped list has a canopy and a floor and almost nothing between the two.
4. **Kapok and strangler fig.** Two trees that are recognisably rainforest at a distance rather than generically green, and both are tree-generator work.
5. **The birds.** Toucan, hornbill and turaco are pure authoring on a generator that has already produced twelve species, and they carry the biome's colour better than anything else that can be built today.
6. **Rocks last.** Two are already shipped and rock is the least distinctive part of a rainforest — it is mostly hidden under plants.

Not on the list and worth naming: **lianas and epiphytes**. Hanging vine curtains and branch-mounted bromeliads are a large part of what makes this biome look like itself, and there is no kind for either in `kinds.py` and no gap token offered for one. That is an open question for whoever owns the kind list, not a species that has been forgotten.

## Where the numbers come from

Every size in this file is an approximate typical adult figure from general knowledge. Nothing here is measured, and nothing here is sourced — no paper, dataset or reference was consulted, and the only citations in this document are repo paths and line numbers, each of which was opened and read. Sizes are good enough to choose a lattice and to size a silhouette against its neighbours; they are not good enough to quote. Anything a spec is authored from should be checked before it is written into that spec's `notes`.

Two further honesty notes:

* This biome is not a place, it is a climate band, and the species above are drawn from three continents that share it — a jaguar and a tiger and a gorilla never meet. That is a deliberate consequence of a climate-gated world and not an error, but it does mean the list should not be read as a fauna of anywhere real.
* Shipped sizes are left exactly as they are. Where an authored size differs from the animal — the kingfisher at 20 cm against a real 17 — the difference is recorded in the row and in the spec's own notes, and it is a decision, not a mistake to correct.
