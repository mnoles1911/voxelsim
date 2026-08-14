# Savanna — master species list

**Biome id 6** — `vxc::BiomeId::SAVANNA`, `voxel-core/include/voxelcore/biome.h:30`; mirrored `asset-forge/forge/biomes.py:69-70`.

| | |
|---|---|
| Climate envelope | precipitation between 400 and 1600 mm/yr, mean annual temperature >= 18 C, AND precipitation seasonality (bio_15, the coefficient of variation of monthly rainfall) >= 70% — a marked dry season of about four months (`biome.h:232-233`, thresholds at `biome.h:135`, `biome.h:137`, `biome.h:121`, `biome.h:184`) |
| Surface material | savanna grass — `MAT_SAVANNA_GRASS`, `biome.h:247` |
| Share of land (shipped world) | **20.76%** over 289 tiles, the second largest biome, with tile `2_-8` at 100.0% (`docs/measurements/biome-screenshot-targets-2026-08-01.txt`). An earlier 121-tile pass in the same file read 18.69% and ranked it third; **use the 289-tile column**, which that file's own two corrections say supersedes it. |
| Water present | yes — seasonal rivers, waterholes and floodplains |
| Asset kinds hosted | tree, bush, rock, grass, reed, flower, fish, cetacean, bird (`biomes.py:69-70` via `_FLIES`, `biomes.py:50`) |

This is the opposite problem to rainforest. Savanna is everywhere — a fifth of the land, and one tile in the shipped world measures 100% savanna — so it is the biome the player crosses rather than visits, seen at long range across open ground with a very high sky. What that means for the asset list is that silhouette is nearly the whole job: a single tree against the horizon, a herd at 200 m, a rock standing alone. Ground detail matters less than in any other vegetated biome, and the count of distinct tree and animal shapes matters more, because a savanna with three tree species in it reads as wallpaper.

Two facts worth carrying while authoring. First, this gate was rewritten at worldgen v22 (`biome.h:139-184`): the old version asked for temperature seasonality rather than precipitation seasonality, and the two conditions it demanded were mutually unsatisfiable, so savanna was dead code for fourteen versions and no tile in the world was ever savanna. Second, having been fixed, it now over-fires — up to 55% of land on some seeds — and it does so at temperate forest's expense, which is the biome directly beneath it in the gate order. So this list will be seen a great deal, possibly in places that ought to look temperate, and it should be built for a wide, dry, open landscape rather than for a narrow tropical band.

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
| Savanna acacia | Bare trunk splitting high into a flat wide crown, the crown a thin horizontal plate two or three times as wide as it is deep. | 8 | `shipped: savanna-acacia` |
| Baobab | Enormously fat smooth grey barrel of a trunk tapering upward, topped with a short stubby branch spray that reads as roots in the air. | 13 | `shipped: baobab` |
| Desert dead | Bare pale bleached trunk and broken limbs, no foliage at all, one or two snapped stubs. | 6 | `shipped: desert-dead` |
| Hawthorn scrub | Low dense tangle branching from near the ground, twiggy, dark, wider than it is tall. | 3.8 | `shipped: hawthorn-scrub` |
| Marula | Rounded dense crown on a single short trunk, mottled grey bark flaking to pale patches, crown roughly as wide as the tree is tall. | 10 | `queued` |
| Sausage tree | Broad heavy spreading crown and thick low branches, with long dark cylindrical fruits hanging on cords well clear of the foliage. | 15 | `queued` |
| Jackalberry | The big riverine tree: tall dense dark evergreen dome on a heavy fluted trunk, often standing out of a termite mound. | 18 | `queued` |
| Fever tree | Acacia shape but unmistakable in colour — smooth powdery lime-yellow bark on trunk and branches, fine feathery crown, grows in stands near water. | 12 | `queued` |
| Mopane | Small crooked trunk, open sparse crown of paired leaflets that read as butterfly shapes, drops its leaves in the dry season leaving a grey twig cage. | 10 | `queued` |
| Doum palm | The only branching palm — the trunk forks two or three times, each fork ending in a crown of stiff fan leaves. | 12 | `queued` |

Generator note: the tree generator's three growth models (`kinds.py:66-71`) cover this table comfortably. The flat acacia crown and the doum palm's repeated fork are both whorl work; the fan crown is frond; everything else is colonize. Nothing in this table needs new code.

## Rock types

All nine are shipped. Block size is the authored `block_size_m` read from each spec in `asset-forge/specs/`.

| Rock | Voxel-artist description | Block size (m) | Status |
|---|---|---|---|
| Banded sandstone ledge | Low stepped shelf with horizontal bedding lines about 35 cm apart, each band a slightly different ochre, undercut along the softest band. | 1.2 | `shipped: banded-sandstone-ledge` |
| Cross-bedded butte | Standing block whose internal layers run at an angle to the ground and cut across each other in sweeping curves rather than lying flat. | 1.2 | `shipped: cross-bedded-butte` |
| Desert mesa block | Flat-topped square-shouldered block with thick beds and a hard resistant cap over softer, more eroded rock below. | 1.2 | `shipped: desert-mesa-block` |
| Exfoliating dome | Rounded whaleback shedding curved shells off its surface like a peeling onion, pale grey with fresher rock exposed under each flake. | 1.2 | `shipped: exfoliating-dome` |
| Fractured outcrop | Rock broken into interlocking angular blocks along two or three joint directions, each block sitting slightly proud of its neighbours. | 1.5 | `shipped: fractured-outcrop` |
| Balanced rock | A large block resting on a much narrower waisted pedestal — the notch, not the block, is the shape. | 1.2 | `shipped: hero-balanced-rock` |
| Limestone slab | Thin flat-bedded pale grey slabs, cleanly parted, edges sharp rather than rounded. | 1.4 | `shipped: limestone-slab` |
| Tafoni sandstone | Blocky rock hollowed by cavities that eat back under a hard outer rind, leaving a thin shell pierced by rounded holes. | 1.2 | `shipped: tafoni-sandstone` |
| Ventifact boulder | Small hard boulder cut by wind-blown sand into flat facets meeting in sharp keel edges, polished on the windward faces. | 1.2 | `shipped: ventifact-boulder` |

Landform gap: there is no **granite kopje** — the rounded boulder-pile inselberg standing out of flat grass — and no **termite mound**, and between them they are the two most recognisable objects on a savanna plain that are not a tree. Both look like rock-generator work (the kopje is an accretion of large part-buried lumps; the mound is a single fluted spire with buttresses at its base), and neither is a species, so both are named as landforms here rather than invented into the table above.

## Flowers

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Desert bloom | Sparse low leaves and one or two open blooms on short stems, held close to the ground. | 0.35 | `shipped: desert-bloom` |
| Fire lily | A single bare stem straight out of bare ground with no leaves at all, topped by a cluster of flame-red trumpet flowers — appears after burns. | 0.4 | `queued` |
| Wild gerbera | Flat rosette of lobed leaves pressed to the ground with two or three long bare stalks each carrying one large flat daisy head, orange or cream. | 0.3 | `queued` |
| Devil's thorn ⚠ | A flat radiating mat of paired leaflets running along the ground with small bright yellow five-petal flowers set into it; no height to speak of. | 0.1 | `queued` |
| Aloe | A rosette of thick pointed grey-green leaves curving out and back, dead brown leaves skirting the base, and one or two branched spikes of dense orange-red flowers held well above it. | 0.9 | `gen: succulent` |

⚠ Devil's thorn: 10 cm tall on the 5 cm ground-cover lattice is two voxels, and the flower itself is smaller than one. It cannot be an individual plant. Author it as a low mat with colour flecks, or leave it to the ground material.

On `gen: succulent`: the closest existing model is **frond** (`kinds.py:66-71`), which already builds a crown of long arching blades from a single base — that is structurally what an aloe rosette is. The differences are that the leaves are thick rather than flat, they curve back on themselves, and the flower spike is a separate stiff stem through the middle. That may be reachable by pushing frond parameters, and it is worth an afternoon of trying before anyone writes a generator. Marked `gen:` here because thick self-curving leaves are not something the current parameters expose, not because the shape is alien to the machinery.

## Ground cover

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Dry tussock | Tight clump of stiff pale straw-coloured blades standing more upright than green grass, with bare soil visible between clumps. | 0.62 | `shipped: dry-tussock` |
| Pampas plume | Tall stand of stems each carrying a large soft feathery seed plume well above the leaves. | 1.7 | `shipped: pampas-plume` |
| Red oat grass | The signature savanna grass: fine arching blades in loose tufts that cure from green to a strong red-brown, with slender dark awned seed heads leaning all one way. | 0.8 | `queued` |
| Elephant grass | A dense wall of thick cane-like stems two to three times head height with broad drooping leaves — used as a stand, not as scattered tufts. | 3.0 | `queued` |
| Floodplain sedge | Stiff triangular-sectioned stems in wet ground, sharply upright with narrow leaves near the base and a small brown seed cluster at the top. | 0.7 | `queued` |

## Bushes / shrubs

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Desert shrub | Low open woody shrub, sparse small leaves, more twig than foliage. | 1.0 | `shipped: desert-shrub` |
| Sicklebush | Dense thorny shrub with fine feathery leaves and hanging two-tone flower spikes — pink at the top, yellow at the hanging end. | 3.0 | `queued` |
| Buffalo-thorn | Zigzag branches with paired thorns at each kink, one thorn straight and one hooked, glossy three-veined leaves, forms an impenetrable rounded mass. | 3.0 | `queued` |
| Wild sage bush | Grey-green multi-stemmed shrub with narrow felted leaves, silvery overall, holding a fluffy off-white seed mass at the branch tips. | 2.5 | `queued` |
| Candelabra euphorbia | Leafless succulent shrub-tree: a short thick stem carrying ranks of upright ribbed green columns that step outward and upward in tiers. | 4.0 | `gen: succulent` |

On the euphorbia specifically: the model that comes closest is **whorl**, which already places branches in rings up a stem and is what the acacia and the doum palm will use. A candelabra euphorbia is very nearly a whorl tree whose branches are ribbed vertical columns instead of tapering woody limbs, and whose crown is those columns rather than foliage. If the whorl model can be given a branch profile that is a fluted column and a foliage setting of none, this row becomes `queued` and the succulent gap shrinks to the aloe rosette alone.

## Birds

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Grey heron | Tall, long-necked, kinked neck folded back in rest, dagger bill, grey above and white below with a black eye-stripe trailing into a crest. | 1.0 | 1 cm | `shipped: grey-heron` |
| Mallard duck | Heavy-bodied, flat-backed, broad flat bill; drake with a bottle-green head, white neck ring and grey body, duck plain mottled brown. | 0.58 | 1 cm | `shipped: mallard-duck` |
| Common kestrel | Small falcon with long pointed wings and a long square tail, rufous back with fine black spots, grey head and tail on the male, dark moustache stripe. | 0.34 | 1 cm | `shipped: common-kestrel` |
| Rock pigeon | Round full-chested body, small head, short bill, grey with two dark wing bars and a green-purple neck sheen. | 0.33 | 1 cm | `shipped: rock-pigeon` |
| Eurasian hoopoe | Pinkish body with boldly black-and-white barred wings and tail, long thin down-curved bill, and a tall fan crest that opens like a hand. | 0.27 | 1 cm | `shipped: eurasian-hoopoe` |
| Barn swallow | Small, streamlined, very long forked outer tail streamers, blue-black above with a red throat and pale underparts. | 0.26 | 1 cm | `shipped: barn-swallow` |
| Common starling | Compact, short-tailed, straight pointed bill, black with a green-purple gloss and pale spotting in winter. | 0.21 | 1 cm | `shipped: common-starling` |
| Common ostrich | Enormous: bare pink-grey neck and legs, a small flat head on a long neck, a rounded feather mass of a body carried very high; male black with white wing and tail plumes. | 2.4 tall | 5 cm | `queued` |
| Secretary bird | An eagle's head and hooked bill on crane's legs, grey body with black thighs and a spray of long loose quills off the back of the head, and two very long central tail feathers. | 1.4 tall | 2 cm | `queued` |
| Lilac-breasted roller | Chunky small bird with a heavy straight bill and two thin tail streamers, and the colour hero of the biome — lilac breast, turquoise belly, deep blue wing panel, brown back. | 0.36 | 1 cm | `queued` |

Range note: six of the seven shipped birds here are a Palearctic set, and they read European rather than tropical. The **mallard** is the marginal one — a mallard on an African waterhole is a stretch, and it is in this biome because the biome is defined by climate rather than by continent. Nothing is wrong with the specs; it is worth knowing that the shipped bird list currently makes this biome look like a hot Mediterranean summer rather than a savanna, and that the three queued rows above are what change that.

Lattice note: the ostrich is the first bird in the library big enough to move off 1 cm. At 2.4 m its smallest identifying feature is the bare neck, roughly 10 cm thick, which holds at 5 cm; the secretary bird's head quills are around 2 cm thick and pull it to a 2 cm lattice despite the bird standing 1.4 m.

## Land animals

Nothing in this table is shipped, and nothing in it can be built today: there is no quadruped generator anywhere in `kinds.py:66-135`. Savanna is where that gap costs the most, because on open ground at long range the animals *are* the biome.

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| Plains zebra ⚠ | Stocky pony shape with a short stiff upright mane and a tufted tail, white with broad black stripes that run vertical on the neck and body and turn horizontal on the rump and legs. | 2.3 head-body / 1.3 at the shoulder | 2 cm | `gen: quadruped` |
| Reticulated giraffe ⚠ | Extreme proportions: forelegs longer than hind, back sloping steeply down to the rump, neck as long as the body, two short skin-covered horns; large flat orange-brown plates separated by narrow white lines. | 5.0 tall / 3.0 at the shoulder | 5 cm | `gen: quadruped` |
| African bush elephant | The biggest silhouette in the world: high domed head, enormous fanned ears reaching the shoulder, curved outward-sweeping tusks, hollow back, pillar legs. | 6.0 long / 3.2 at the shoulder | 5 cm | `gen: quadruped` |
| White rhinoceros | Slab of a body with a pronounced neck hump, square wide-lipped muzzle carried low to the ground, a long front horn and a short second one behind it. | 3.8 long / 1.8 at the shoulder | 5 cm | `gen: quadruped` |
| Hippopotamus | Barrel on stumps, enormous blunt head nearly a third of the body, tiny ears and eyes set on top of the skull, grey-pink and hairless. | 3.5 long / 1.5 at the shoulder | 5 cm | `gen: quadruped` |
| Cape buffalo | Heavy black ox with the head carried low, and a helmet of fused horn across the forehead from which the horns drop out, down and then hook up. | 2.8 head-body / 1.5 at the shoulder | 5 cm | `gen: quadruped` |
| Blue wildebeest | Front-heavy and awkward: high humped shoulders falling away to low hips, long blunt cow-like head hung low, dark vertical stripes on the forequarter, a black beard and a long black tail. | 2.2 head-body / 1.4 at the shoulder | 2 cm | `gen: quadruped` |
| Greater kudu ⚠ | Tall grey-brown antelope with a white chevron between the eyes, six to ten thin white vertical stripes on the flanks, a throat fringe, and long horns in two and a half open spirals. | 2.3 head-body / 1.4 at the shoulder | 2 cm | `gen: quadruped` |
| Gemsbok | Blocky pale fawn antelope with a black side-stripe separating flank from belly, black leg stripes, a bold black-and-white face mask, and two very long straight horns swept back like spears. | 2.0 head-body / 1.2 at the shoulder | 2 cm | `gen: quadruped` |
| Impala | Light and leggy, rich red-brown above shading through tan to white below, with a black vertical stripe down each side of the tail and lyre-shaped ringed horns on the male. | 1.4 head-body / 0.9 at the shoulder | 2 cm | `gen: quadruped` |
| Warthog | Low grey barrel on short legs, disproportionately large flat head with paired facial warts, upward-curving tusks, a spiky mane along the spine and a thin tail carried straight up when running. | 1.3 head-body / 0.7 at the shoulder | 2 cm | `gen: quadruped` |
| Lion | Deep-chested heavy cat with a long straight back, plain tawny coat and a black tail tuft; the male carries a mane that thickens the whole front of the silhouette and darkens with age. | 2.0 head-body / 1.1 at the shoulder | 5 cm | `gen: quadruped` |
| Leopard ⚠ | Long low cat with a heavy head and a very long thick tail, short legs, golden coat covered in dark rosettes that enclose a tawny centre. | 1.5 head-body / 0.7 at the shoulder | 2 cm | `gen: quadruped` |
| Cheetah ⚠ | Built like a greyhound: deep narrow chest, small round head, long thin legs, tan with small solid black spots and a black tear line running from eye to mouth. | 1.3 head-body / 0.85 at the shoulder | 1 cm | `gen: quadruped` |
| Spotted hyena | Sloping back from high heavy shoulders to low hindquarters, big rounded ears, thick neck, sandy coat with dark irregular blotches and a short bushy tail. | 1.4 head-body / 0.85 at the shoulder | 2 cm | `gen: quadruped` |
| African wild dog | Lean long-legged dog with enormous round ears, a white-tipped tail, and a coat of large irregular patches of black, tan and white, different on every individual. | 1.1 head-body / 0.75 at the shoulder | 2 cm | `gen: quadruped` |
| Chacma baboon | Dog-like muzzle on a grey-brown body, high shoulders, tail carried in a sharp kink up then down, bare dark face and heavy brow. | 0.9 head-body / 0.7 at the shoulder | 2 cm | `gen: quadruped` |
| Meerkat | Small, slim, upright when watching — a vertical stance on hind legs with a straight thin tail as a prop; pale tan with dark eye patches and faint dark bands across the back. | 0.3 head-body, 0.2 tail | 1 cm | `gen: quadruped` |
| Nile crocodile | Very flat and wide, sprawling legs, a long broad snout, and a tail more than half the total length carrying a double row of raised scutes that merges into a single crest. | 4.5 total | 5 cm | `gen: quadruped` |
| Nile monitor lizard | Sprawling large lizard, long neck, whip tail twice the body length, dark olive with rows of yellow spots and bands across the back. | 1.6 total, 0.6 head-body | 2 cm | `gen: quadruped` |
| Leopard tortoise | High domed shell with pronounced raised plates, pale yellow ground broken by black radiating blotches, thick elephantine front legs. | 0.5 shell | 1 cm | `gen: chelonian` |
| African rock python | Very heavy-bodied snake, thickest a third of the way back, blotched brown and olive with a dark arrowhead mark on the head and a pale line under the eye. | 4.5 long | 5 cm | `gen: serpentine` |
| Emperor scorpion ⚠ | Flat wide body with heavy claws held forward, eight legs in a low radial spread and a segmented tail arched over the back with a bulb and a sting at the end; glossy black. | 0.18 | 1 cm | `gen: arthropod` |

⚠ Zebra: the stripes are roughly 5-8 cm wide on a 2.3 m body. At a 5 cm lattice each stripe is one voxel and the pattern collapses; at 2 cm it is three voxels and reads. The stripe, not the animal, chooses the lattice — this is the clearest case of the house rule in either file.

⚠ Giraffe: the animal is huge and would sit happily at 10 cm, but its pattern is defined by the *gaps* — narrow pale lines about 3-5 cm wide between large plates. At 5 cm the lines are one voxel; the honest options are to widen the lines above life size and say so in the spec notes, or to accept a 5 cm lattice and draw the plates with soft edges rather than as a network. Do not go to 2 cm for this — a 5 m animal at 2 cm is 250 voxels tall and it is not worth it.

⚠ Kudu: two features fight. The flank stripes are around 2 cm wide, and the horns are 4-6 cm thick at the base and thinner up the spiral. At 2 cm the stripes are one voxel and the horn is two — both below what reads. This one needs the fix the library already used for small birds: draw the stripes wider and the horns thicker than life, and write the decision into the spec's `notes`. Pretending a 2 cm lattice holds a 2 cm stripe is how a spec ends up with a number nobody can reproduce.

⚠ Leopard: rosettes are of the same order as a zebra's stripes, with the added difficulty that a rosette is a ring rather than a band — it needs an outside, a wall and an inside, so three voxels across is the floor rather than a comfortable target. 2 cm is the coarsest that will hold it.

⚠ Cheetah: the spots are 2-3 cm solid dots on a 1.3 m animal, which is finer than the leopard's problem because a dot has no interior to preserve but is also much smaller. 1 cm makes each spot two to three voxels and puts the whole animal at about 130 voxels long, which is within the range the library already draws birds at. The tear line — under 1 cm wide — will have to be drawn thicker regardless.

⚠ Scorpion: the legs and the tail segments are around 1 cm thick, so at the 1 cm lattice each is one voxel. Author the legs and tail at 2-3 cm and record it. Note the floor this implies for the biome: dung beetles, termites and anything else under about 4 cm cannot be a voxel asset at any lattice and belong to a particle or decal system instead.

A note on `gen: serpentine`: this is probably the least work of the missing generators, because it may not be a new generator at all. The fish generator already lofts `river-eel` at a depth ratio of 0.085 — a body about seventeen times longer than it is deep — with the dorsal and anal fins collapsed into one continuous low ridge. Turn the fins off and take the tail to a point and a rock python is closer to shipped than any mammal in this table.

## Fish

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Golden carp | Deep-bodied, high-backed, long low dorsal fin running most of the back, forked tail, uniform orange-gold. | 0.40 | 1 cm | `shipped: golden-carp` |
| Mirror carp | Big deep body with a heavy shoulder and a blunt head; scaleless except for a few large irregular plate scales scattered along the flank and back. | 0.90 | 2 cm | `shipped: mirror-carp` |
| Mud catfish | Wider than deep, flat-bottomed, big flat head a third of the length, four barbels off the snout, mottled brown. | 0.38 | 1 cm | `shipped: mud-catfish` |
| Pale minnow | Small, plain, slim, forked tail, silver with a faint dark lateral line — the shoaling filler species. | 0.20 | 1 cm | `shipped: pale-minnow` |
| African sharptooth catfish | Long eel-like body with a very long unbroken dorsal fin, flat bony wide head, four pairs of barbels, dark marbled grey-brown over a cream belly. | 1.2 | 2 cm | `queued` |
| Nile tilapia | Deep oval compressed body, spiny dorsal along the whole back, and five to seven dark vertical bars on the flank with faint bars carried onto the tail. | 0.4 | 1 cm | `queued` |
| Nile perch | Large-bodied, blunt-headed, silver with a distinct notch splitting the dorsal fin in two and a large rounded tail; the biggest fish in the biome. | 1.5 | 2 cm | `queued` |
| African lungfish | Long cylinder with pointed rope-like paired fins instead of paddles, a continuous fin ridge around the tail, small eyes, plain grey-brown. | 1.4 | 2 cm | `queued` |

Range note: **golden carp** and **mirror carp** are Eurasian, and where they occur in warm seasonal rivers it is usually because somebody put them there. Left as shipped and left assigned here — the biome is a climate band, not a continent — but flagged so it is a known choice and not an oversight.

Cetaceans are hosted by this biome (`biomes.py:69-70`) and there are none in the table, which is correct: no whale or dolphin lives in a seasonal savanna river. The cetacean generator has nothing to do here.

## Build priority

1. **The quadruped generator.** Twenty-three rows are blocked on it, and it costs more here than in any other biome: this is 20.76% of land, it is open ground where the horizon is visible, and a savanna with no animals on it looks like a mown field. Zebra, wildebeest and impala first — herd animals in numbers do more per asset than any single hero species.
2. **The three queued birds.** Ostrich, secretary bird and lilac-breasted roller are pure authoring on a generator with seven species already shipped in this biome, and they are the fastest way to stop the biome looking Palearctic.
3. **Marula, jackalberry and fever tree.** Three tree silhouettes against a big sky, generator already exists. Tree variety is what stops a biome this large reading as wallpaper, and there are only four tree specs in it today.
4. **Red oat grass.** One ground-cover spec changes the colour of a fifth of the world's land, because savanna is seen as a plane of grass with things standing on it. Cheap, and disproportionately visible.
5. **The kopje and the termite mound.** Both look like rock-generator work, both are landforms rather than species, and both are more recognisably savanna than any of the nine shipped rocks.
6. **`gen: serpentine` as a fish variant**, for the reason above — one animal, nearly free.
7. **The succulents last.** Try the frond and whorl models on the aloe and the euphorbia before writing anything; the euphorbia in particular may already be reachable.

## Where the numbers come from

Every size in this file is an approximate typical adult figure from general knowledge. Nothing here is measured, and nothing here is sourced — no paper, dataset or reference was consulted, and the only citations in this document are repo paths and line numbers, each of which was opened and read. Sizes are good enough to choose a lattice and to size a silhouette against its neighbours; they are not good enough to quote. Anything a spec is authored from should be checked before it is written into that spec's `notes`.

Two further honesty notes:

* The stripe and spot widths used in the ⚠ notes are the weakest figures in the file. They are order-of-magnitude estimates, and they are doing real work — they are what picks 2 cm over 5 cm for six animals. If any of those lattices is going to be argued about, that estimate is the thing to check first, and the check is cheap: draw the pattern at both lattices and look at it, which is how this project settles shape questions anyway.
* Shipped sizes are left exactly as they are. Where an authored size differs from the real animal the difference belongs in the row and in the spec's own notes; it is a decision made for the lattice, not a mistake to correct.
