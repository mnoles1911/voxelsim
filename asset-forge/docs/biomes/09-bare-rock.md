# Bare rock — master species list

**Biome id 9** — `vxc::BiomeId::BARE_ROCK`, `voxel-core/include/voxelcore/biome.h:40`; mirrored `asset-forge/forge/biomes.py:79`.

| | |
|---|---|
| Gate | MORPHOLOGY, and it fires before anything else on dry land. `classifyBiome` (`biome.h:220`) returns this for any ground steeper than a 70% grade — about 35 degrees, which is the angle of repose for soil and scree, above which loose material does not stay put. That is not a tuned number, it is the physical reason a cliff is bare (`biome.h:52-71`). Climate is never consulted: a face this steep in a wet mild valley is bare rock, exactly as a face this steep at 4,000 m is. |
| Surface material | `MAT_ROCK`, unconditionally (`biome.h:249`). One look, no elevation split. |
| Share of land (shipped world) | Absent from the census, and not because there is none. `docs/measurements/biome-screenshot-targets-2026-08-01.txt:24-26` says it directly: "BARE_ROCK is absent from this table only because world_map.py cannot evaluate its slope gate; it is a client-side cliff test and appears in game." The census classifies coarse tiles, which have no slope at the resolution the gate reads. So the honest statement of this biome's share is: unknown, non-zero, and only observable in the client. |
| Water present | No standing or flowing water is classified here. Water bodies take the biome of the land around them (`biomes.py:37-42`), and land around water is by definition not a 35-degree face. Waterfalls and sea cliffs touch this biome; they are not in it. |
| Asset kinds hosted | `rock` and `bird`. That is the entire tuple (`biomes.py:79-81`), and `plantable=False`. |

A cliff face, anywhere in the world, at any altitude, in any climate. It is the youngest biome in the enum — appended at worldgen v8 to stop steep low sea cliffs classifying as alpine permafrost, back when the cliff gate fired at ~11 degrees and the surface material split rock from permafrost on elevation alone (`biome.h:33-39`). Because the gate is about steepness rather than place, this one biome covers a granite big wall, a chalk sea cliff, a limestone gorge and the headwall above a glacier, and the asset list has to cover that whole range of lithology rather than one climate's worth. There are exactly two things to build here and one of them is nearly finished.

## How to read the tables

**Status** is one of:
* `shipped: <spec-name>` — a spec already exists in `asset-forge/specs/`.
* `queued` — a generator exists for this kind; this is authoring work only.
* `gen: <name>` — needs a generator that does not exist. See the index for the gap list.
* `host: <kind>` — the species is right for the place but the biome's `hosts` tuple in `biomes.py` does not admit that kind yet; that is an engine-side change, not an authoring one.

**Lattice** (animal tables only) is the voxel size the asset should be authored at, by the house rule in `asset-forge/forge/kinds.py:29-58`: a species is drawn at the COARSEST voxel size at which its smallest identifying feature is still about three voxels across. Trees and rocks are not listed with a lattice because they join the world's terrain grid and are 10 cm and nothing else; ground cover, bushes, flowers and reeds are 5 cm.

**⚠** marks a species whose defining feature is at or below what the lattice will hold, explained in the note under its table.

## Trees

**None, and here is why.** The cliff gate runs ahead of the climate table (`biome.h:216-221`, and the ordering note at `biomes.py:8-14`), so a column is bare rock before its temperature and rainfall are ever read. `plantable=False` (`biomes.py:80`) and `tree` is not in the `hosts` tuple, so no tree spec can be weighted here even by mistake. The physical reason is the same one that sets the gate: above 35 degrees loose material does not stay, so there is no soil, and a tree needs soil more than it needs climate.

Real cliffs do carry the occasional rooted-in-a-crack pine or fig, leaning out of the face. That is a genuine feature of cliffs and it is deliberately NOT requested here. It would need `tree` added to `hosts`, a crack-seeking placement rule, and a horizontal growth model — three changes to make one rare prop, against a biome whose two hosted kinds are not yet finished. Revisit after the rock table is closed out.

## Rock types

**This is the richest rock biome in the world and it is already the best-served.** Thirty-two of the library's thirty-four rock specs carry a non-zero `bare_rock` weight — everything except `mossy-forest-boulder` and `river-cobble`, both of which want flat wet ground and neither of which belongs on a face.

What already exists, grouped by what it is:

* **Granite and coarse crystalline** — `granite-boulder`, `veined-granite`, `jointed-granite-tor`, `corestone-tor`, `summit-tor`, `hero-tor-stack`, `exfoliating-dome`, `standing-stone`, `glacial-erratic`.
* **Basalt and volcanic** — `basalt-colonnade`, `hero-basalt-colonnade`.
* **Limestone and karst** — `limestone-slab`, `limestone-pinnacles`, `karren-pavement`, `hero-tsingy-pinnacles`.
* **Sandstone and clastic** — `banded-sandstone-ledge`, `cross-bedded-butte`, `desert-mesa-block`, `tafoni-sandstone`, `honeycomb-tafoni`, `ventifact-boulder`.
* **Arch, stack and free-standing landform** — `desert-arch`, `hero-natural-arch`, `hero-arch-colossal`, `hero-balanced-rock`, `desert-hoodoo`, `hero-sea-stack`, `wave-cut-stack`.
* **Broken and fallen material** — `alpine-scree`, `cliff-fall-block`, `fault-breccia`, `fractured-outcrop`.

So this table is not thirty-two rows of new work. It is six anchors that define the look, and then the forms that are genuinely missing — mostly lithologies whose identity is a SURFACE FABRIC (cleavage, banding, clast texture) rather than a block shape, plus the negative-space landforms a cliff has and a boulder does not.

| Species | Voxel-artist description | Height or block size (m) | Status |
|---|---|---|---|
| Exfoliating granite dome | Smooth convex bulge of pale granite peeling in curved sheets that follow the surface, each sheet a few tens of centimetres thick, with open arcs where a sheet has dropped away. | 3.0 | `shipped: exfoliating-dome` |
| Columnar basalt colonnade | Vertical dark-grey polygonal columns, mostly hexagonal, packed side by side with clean flat faces and horizontal cross-fractures at intervals. | 1.2 | `shipped: hero-basalt-colonnade` |
| Limestone pinnacles | Forest of thin pale grey blades and spikes standing vertically, razor-edged tops, deep slots between them. | 2.0 | `shipped: limestone-pinnacles` |
| Honeycomb tafoni | Sandstone face eaten into a dense mass of rounded cavities separated by thin rock walls, deeper toward the centre of each hollow, pale rims and dark interiors. | 1.4 | `shipped: honeycomb-tafoni` |
| Cliff-fall block | Freshly detached slab lying at the foot of the face, one clean bright unweathered fracture surface against otherwise dark old rock, tilted with a void under an edge. | 2.8 | `shipped: cliff-fall-block` |
| Sea stack | Isolated pillar standing off the cliff line, undercut at the base by wave attack, horizontal bedding continuing across from stack to mainland so the eye reads them as once-joined. | 4.0 | `shipped: hero-sea-stack` |
| Chalk cliff face | Blinding white near-vertical face, soft and blocky rather than fractured, with horizontal dark flint bands running through it and a fan of white rubble at the foot. The whiteness is the asset; nothing else in the library is this bright. | 2.0 blocks | queued |
| Schist cleavage face | Silvery-grey rock splitting along a single strong wavy foliation, so the whole face is made of thin lensy sheets all leaning the same way, glittering with mica. Directionality is the identity — a schist face with random fracture is just a grey rock. | 1.0 | queued |
| Slate face | Flat blue-grey rock cleaving into clean thin plates, all parallel, with sharp straight edges; broken slate stacks in flat plates rather than angular chunks. | 0.8 | queued |
| Gneiss banded face | Coarse crystalline rock with strong alternating light and dark bands, folded and swirled rather than straight, so the banding curves through the block. Two-material asset by definition. | 1.6 | queued |
| Conglomerate face | Matrix of fine rock packed with rounded pebbles and cobbles of other rock, half of them standing proud where the matrix has weathered back. Reads as concrete made by geology. | 1.2 | queued |
| Serpentinite outcrop | Waxy dark green-black rock with a slick polished sheen and pale veining, breaking into smooth rounded lumps rather than sharp blocks. | 1.0 | queued |
| Gypsum / alabaster face | Translucent-looking white to honey rock, softly rounded and fluted by dissolution, with a satin fibrous grain visible on broken faces. | 1.0 | queued |
| Obsidian flow face | Glossy black glass, conchoidal shell-shaped fracture scars across the whole surface, edges bright and sharp, with grey-white banded streaks through the black. | 1.5 | queued |
| Pillow basalt | Stacked rounded lobes like piled sacks, each with a glassy rind and a radially cracked interior, packed with fine material in the gaps. | 0.8 | queued |
| Dolerite dyke | A straight, hard, dark band of rock cutting cleanly across the bedding of everything around it, standing proud as a wall because it resists weathering better than its host. | 1.2 | queued |
| Quartz vein reef | Milky-white blocky quartz standing out as a bright ridge through darker host rock, sugary broken texture, sharply bounded on both sides. | 0.7 | queued |
| Talus cone | Fan of angular debris spreading from the mouth of a gully at the cliff foot, steep and straight-sided, coarse blocks at the toe and fines at the apex. | 8-20 across | `gen: rock` (needs patch/fan scatter placement) |
| Rock glacier | Tongue of blocky debris with a steep 30-degree snout and arcuate ridges and furrows across its surface — a landform that flows, so it has to read as viscous even though it is made of blocks. | 3-15 | `gen: rock` (patch scatter + a flow-form base) |
| Sea-cave mouth | Dark arched void cut into the base of a sea cliff along a weakness, wider at the waterline than above, with wave-polished flared walls. Negative space, not a block. | 4-8 opening | queued |
| Chimney / couloir cleft | Narrow vertical slot cutting into a face, parallel walls a body-width or two apart, choked with jammed blocks at intervals. Reads as a route up the cliff. | 1-3 wide | queued |
| Overhang roof | Section of face that leans out past vertical, forming a horizontal ceiling of rock with a sharp lip; darker and drier under it than around it. The one geometry a boulder generator cannot make and a cliff cannot do without. | 2-6 out | queued |

Notes on this table:

* **The two `gen: rock` rows are not asking for a new kind.** The rock generator makes one lump; a talus cone and a rock glacier are DISTRIBUTIONS of lumps with a landform-scale shape. That is a placement/scatter feature. The tundra/alpine file asks for the same feature for blockfields and patterned ground, so it is one piece of work serving five entries across two biomes.
* **Frost-shattered blockfield** is deliberately not repeated here; it is on the tundra/alpine list, and it is a feature of flat high ground rather than a face, so that is where it belongs.
* **Six of the missing rows are surface-fabric species** — schist, slate, gneiss, conglomerate, obsidian, quartz vein. What identifies each is a directional or two-material texture across the block, not its outline. Before authoring them, check whether the rock generator's material handling can express a foliation direction at all; if it cannot, these six are one shared generator feature and not six authoring jobs.
* **Three rows are negative space** — sea cave, chimney, overhang. A generator built on accreted lumps makes convex things. These need subtraction, and they are the highest-value additions in the table, because a cliff without an overhang or a cleft reads as a wall.

## Flowers

**None, and here is why.** `flower` is not in this biome's `hosts` tuple (`biomes.py:79-81`) and `plantable` is `False`. The gate reason is the same as for trees: the cliff test at `biome.h:220` fires before the climate table, so no amount of warmth or rain can put a flowering plant on this column. The physical reason is that a 35-degree-plus face holds no soil by definition — that is what the angle of repose means (`biome.h:52-68`).

Crevice plants — saxifrages, campanulas, ferns rooted in a fissure — are real, and they are the only vegetation a genuine cliff carries. They are not requested. A crevice plant needs `flower` in `hosts`, a fissure-finding placement rule, and it would be a few voxels on a face nobody gets close to. Low value against the rock and bird work that is actually blocking.

## Ground cover

**None as geometry, and here is why.** `grass` and `reed` are not in the `hosts` tuple, and there is no ground here to cover — the biome IS the absence of a soil-holding surface.

The one exception is worth writing down precisely because it is not an object:

| Species | Voxel-artist description | Height or block size (m) | Status |
|---|---|---|---|
| Crustose and foliose lichen crust | Not a plant standing on rock — a coloured skin bonded to it. Crustose: irregular flat patches, chalk-white, sulphur-yellow, rust-orange or black, with crazed edges, following every contour of the surface. Foliose: slightly raised grey-green leafy lobes with curled margins, lifting a few millimetres clear at the edges only. | 0.001-0.005 thick | `gen: lichen` |

**The honest implementation is a material/palette variant on the existing rock generator, not a new kind, and here is why I think so.** A lichen crust is one to five millimetres thick. The finest lattice this project uses anywhere is 1 cm, for small birds (`kinds.py:129-134`), and rocks are locked to the terrain grid at 10 cm because they must be addressable in the world voxel grid, which has exactly one cell size (`kinds.py:29-58`). So a lichen crust is between one tenth and one hundredth of a voxel thick on the lattice it would have to live on. There is no voxel size at which it is geometry, and the house rule — three voxels across the smallest identifying feature — is not merely failed here, it is failed by two orders of magnitude. Meanwhile everything that makes lichen recognisable is colour and patch outline: the orange, the black, the white, the crazed boundary. All of that is expressible as a per-face material variant on a rock spec, which costs a palette and a mask and no new generator at all.

The counter-argument — that foliose lichen genuinely lifts off the rock and so is real relief — is true and does not survive the numbers: the lift is a few millimetres. `gen: lichen` is therefore recorded as a gap token so the need is tracked, with the recommendation that whoever picks it up ships it as rock materials and closes the token rather than building a kind.

## Bushes / shrubs

**None, and here is why.** `bush` is not in the `hosts` tuple (`biomes.py:79-81`), `plantable` is `False`, and the cliff gate fires before climate is read (`biome.h:220`). Same reasoning as trees and flowers: no soil above the angle of repose, so nothing woody roots here. There is no request attached to this section — a shrub on a cliff face is rarer than a tree on one, and the tree case was already judged not worth the three changes it needs.

## Birds

**The whole point of this biome for animate assets.** `biomes.py:45-50` records why birds are hosted where nothing else is: the two biomes carved out as exceptions — one for having no ground, one for being too steep to stand on — are both good bird habitat, and birds are the first kind for which that is true. Cliffs are not marginal bird habitat, they are premium: they are where seabird colonies, raptor eyries and cliff-nesting corvids all are, precisely because nothing can walk up to a nest on a 40-degree face.

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| Golden eagle | Large dark-brown raptor with a pale golden nape, long broad wings with deeply slotted primaries held in a shallow V, long square tail. | 0.85 | 1 cm | `shipped: golden-eagle` |
| Common raven | All-black heavy corvid, thick deep bill, shaggy throat feathers making the neck look ragged, wedge-shaped tail. | 0.64 | 1 cm | `shipped: common-raven` |
| Common buzzard | Mid-sized broad-winged raptor, brown above with a variable pale chest band, short rounded tail, wings held in a shallow V when soaring. | 0.52 | 1 cm | `shipped: common-buzzard` |
| Common kestrel | Small slim falcon, rufous back with black flecks, long pointed wings and a long tail with one broad black band at the tip; hovers with the tail fanned. | 0.34 | 1 cm | `shipped: common-kestrel` |
| Herring gull | Large gull, pale grey back and white body, black wingtips with white spots, heavy yellow bill with a red spot on the lower mandible. | 0.60 | 1 cm | `shipped: herring-gull` |
| Rock pigeon | Compact grey pigeon with two bold black wingbars, a white rump, and a green-purple iridescent neck patch. The ancestral cliff bird, and the reason city pigeons nest on ledges. | 0.33 | 1 cm | `shipped: rock-pigeon` |
| Rock ptarmigan | Compact round game bird, small head, short bill, feathered legs; white in winter, mottled grey-brown in summer, black tail always. | 0.36 | 1 cm | `shipped: rock-ptarmigan` |
| Peregrine falcon | Powerful anchor-shaped falcon, slate-blue above, white below with fine dark barring, and a broad black moustache stripe below the eye. Wings are scythe-like and swept in a stoop — the stoop pose is a distinct second asset. | 0.45 | 1 cm | queued |
| Griffon vulture | Very large pale sandy-brown vulture with a bare whitish head on a long neck, a white ruff at the neck base, and enormous broad plank-like wings with dark flight feathers and heavy finger slots. | 1.0 | 1 cm | queued |
| Red-billed chough | Glossy black corvid with a long, thin, DOWNCURVED red bill and red legs. The bill curve is the whole identification and it is the entire difference from the yellow-billed alpine chough. ⚠ | 0.40 | 1 cm | queued |
| Wallcreeper | Small ash-grey bird with huge rounded crimson-and-black wings flicked open constantly, and a long fine downcurved bill; climbs vertical faces. The most cliff-specific bird there is — it lives on nothing else. ⚠ | 0.16 | 1 cm | queued |
| Alpine swift | Large swift, scythe-shaped wings much longer than the body, brown with a white belly and a brown breast band; never perches on the ground, only clings to rock. | 0.22 | 1 cm | queued |
| Eurasian crag martin | Small stocky brown martin with a broad squared tail showing a row of small white spots when fanned, and paler underwings. | 0.14 | 1 cm | queued |
| Northern fulmar | Stiff-winged grey-and-white tubenose, thick neck, bull-headed, with a short heavy bill carrying visible tube nostrils along the top. Holds its wings straight and rigid, never bent. ⚠ | 0.47 | 1 cm | queued |
| Black-legged kittiwake | Small clean gull, pure grey back, black legs, and wingtips dipped in solid black as if cut off — no white spots in the black, which is what separates it from every other gull. | 0.39 | 1 cm | queued |
| Common guillemot | Upright cigar-shaped auk, dark chocolate above and white below, narrow pointed bill, tiny wings set far back; stands vertically on a bare ledge with no nest at all. | 0.42 | 1 cm | queued |

⚠ notes:

* **Red-billed chough**: the bill is the only difference from its alpine sibling, and it is ~1 cm thick over a 5 cm curve. At the 1 cm lattice that is a one-voxel-thick line — present, but any error kills the identification. Author the bill slightly thickened and record it; the colour does most of the work, so this one is close to a palette problem rather than a geometry one.
* **Wallcreeper**: a 16 cm bird whose identity is a 3 cm needle bill and the crimson wing panels. The panels are palette and fine. The bill is one voxel at 1 cm and needs the above-life-size treatment small birds already get (`kinds.py:129-134` draws birds to read at 20-90 voxels long; at 16 cm this bird is at the bottom of that range before the bill is even considered).
* **Northern fulmar**: the tube nostrils are the family mark and they are ~1 cm on the bill. They will not survive at 1 cm and should not be attempted — the stiff straight-winged flight posture identifies this bird at any distance the player will see it, and posture is free.

Not a lattice question: the rock ptarmigan's seasonal white/mottled plumage and the guillemot's summer/winter head pattern are material variants on one geometry.

## Land animals

**The engine hosts no land animals here.** `hosts` for this biome is `("rock", "bird")` and nothing else (`biomes.py:79-81`). There is also no quadruped generator anywhere in the project — `kinds.py:66-135` has trees, bushes, rocks, three tuft kinds, fish, cetacean and bird, and nothing with four legs. So every row below is blocked twice over, and neither block is authoring work.

**And the engine is wrong about this, which is the finding worth recording.** The gate that defines this biome is the angle of repose for soil and scree, 35 degrees (`biome.h:52-71`). That is the angle above which loose material will not stay put. It is emphatically NOT the angle above which a hoofed animal will not stay put — the entire adaptive point of a cliff-dwelling ungulate is standing where the substrate cannot. Ibex on a dam wall, chamois on a couloir, mountain goat on a headwall, rock hyrax on a kopje face: these animals are found on ground steeper than 35 degrees, routinely, and a player who sees a sheer face with birds on it and nothing standing on it is seeing a world that is subtly wrong in a way they will feel and not name.

**So this section is a REQUEST, not a build list: add `quadruped` to this biome's `hosts` tuple in `biomes.py:79-81`.** That is a one-line engine-side change, it does not touch `biome.h` or the classifier or worldgen determinism (nothing in `voxel-core` reads `hosts`), and it does not require a worldgen version bump. It should be made when the quadruped generator exists and not before, so the tuple never advertises a kind that cannot be built. The species below are the argument for the change, recorded so the reasoning survives; they are **not** shipped-ready and should not be treated as a queue.

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| Alpine ibex | Compact stocky goat, forequarters heavier than hind, dun-grey with a dark dorsal stripe, and thick backswept horns ridged with transverse knobs curving in a single arc to two-thirds of body length. | 1.5 long / 0.9 shoulder; horn to 1.0 | 5 cm | `host: quadruped` |
| Chamois | Slender goat-antelope, tan with a black dorsal stripe and a white face split by a bold black eye-to-muzzle stripe, short vertical horns hooked sharply backwards at the tip. ⚠ | 1.2 / 0.75 | 2 cm | `host: quadruped` |
| Mountain goat | Heavy white shaggy goat with a pronounced shoulder hump, long chin beard, and short black slightly backward-curving horns; black hooves and muzzle are the only dark points on it. | 1.5 / 1.0 | 5 cm | `host: quadruped` |
| Rock hyrax | Guinea-pig-shaped, tail-less, grey-brown, with a blunt face, small round ears and short legs; rubber-padded feet let it stand on smooth stone. Looks like a rodent and is not one. | 0.5 / 0.25 | 2 cm | `host: quadruped` |
| Barbary sheep | Sandy-brown wild sheep with a long fringe of hair hanging from the throat down between the forelegs, and horns sweeping outward and back in a wide arc. The throat fringe is the silhouette. | 1.5 / 0.95 | 5 cm | `host: quadruped` |
| Klipspringer | Very small antelope standing on the very tips of its hooves, giving a tiptoe stance and an arched back; coarse olive-speckled coat, short straight spike horns, oversized rounded ears. ⚠ | 0.85 / 0.55 | 2 cm | `host: quadruped` |

⚠ and lattice notes, if this is ever unblocked:

* **Horns survive a coarse lattice; the fine points do not.** An ibex horn is 8-10 cm at the base over a metre of arc, so at 5 cm it is two to three voxels thick across a 20-voxel sweep and reads correctly; the transverse knobs on it are ~4 cm and will be lost, which is acceptable because the arc alone identifies the animal. **Chamois** is the marginal case: the horn is ~3 cm and the backward hook at the tip is the identification, so even at 2 cm the hook is about 1.5 voxels and must be thickened deliberately. **Klipspringer** horns are thin spikes ~2 cm thick and are effectively unbuildable at any lattice this project uses — but the tiptoe stance and the arched back identify it without them, so the fix is posture, not resolution.
* The correct move for anything the lattice cannot hold is the one this project already used for small birds: author it above life size and say so in the spec `notes`. Not a finer lattice, and not pretending the feature is there.

## Fish

**None, and here is why.** `fish` is not in the `hosts` tuple (`biomes.py:79-81`), and the deeper reason is how water is classified: a river or lake takes the biome of the LAND AROUND IT, and only open sea below -3 m comes out as `OCEAN` (`biomes.py:37-42`, `biome.h:216-217`). Land around water is flat or gently sloping by definition, so it never passes a 35-degree gate. A cliff can stand over water — a sea cliff, a gorge wall, a plunge pool below a fall — but the water at its foot is classified beach, ocean, or whatever the valley is, and the fish in it belong to that biome's list, not this one.

Seals hauled out at a cliff foot are the one adjacent case, and they are the same story: the rock they lie on is beach or ocean-adjacent, not a 35-degree face. If pinnipeds are ever wanted they belong on the beach and ocean lists under `gen: pinniped`, and are noted here only so the next person does not go looking for them in the wrong file.

## Build priority

1. **Overhang roofs, chimneys and sea-cave mouths.** Three assets, one capability: subtraction. Every rock spec in the library is convex because the generator accretes lumps, and a cliff without a roof, a cleft or a cave reads as a wall with texture on it. This is the largest single improvement available to the biome and it improves all thirty-two existing specs' surroundings, not just three new ones.
2. **Peregrine falcon, griffon vulture, wallcreeper.** Birds are the only living thing this biome can legally contain, the generator is mature, and these three are the cliff specialists — a peregrine stooping past a face and a wallcreeper flicking crimson wings against grey rock are what makes a cliff read as inhabited rather than as terrain.
3. **The six surface-fabric lithologies** (schist, slate, gneiss, conglomerate, obsidian, quartz vein). Check first whether the rock generator can express a foliation direction; if it can, these are six cheap authoring jobs that triple the visual variety of faces. If it cannot, that check is itself the deliverable and turns six jobs into one feature.
4. **Chalk cliff face.** One spec, and it is the only asset in the library that would be white. The palette range of the whole rock library today runs grey to sandstone; chalk is the cheapest way to make one region's cliffs unmistakable.
5. **Lichen as rock materials.** Colour on stone, no new kind, no new geometry, and it makes every existing rock spec look weathered and placed rather than dropped in. Close the `gen: lichen` token by shipping materials, not a generator.
6. **`quadruped`** — the generator, then the one-line `hosts` change. Last, because it is the biggest piece of work in the project and this biome is not the best argument for it; tundra/alpine has twenty entries blocked on the same generator. When it lands, this file's six rows are the reason to also touch `biomes.py:79-81`.

## Where the numbers come from

Every size in this file is an **approximate typical adult figure from general knowledge**. Nothing here is measured and nothing here is sourced — no dataset, no paper, no survey stands behind any figure in these tables, and none is cited because none exists. The rock block sizes are the same kind of estimate: a characteristic scale for the form, chosen so the asset sits correctly against a 10 cm terrain lattice, not a surveyed dimension of any real outcrop.

They are good enough for the one job they have: choosing a lattice, which is a decision about whether a feature lands on one voxel or three, and is insensitive to a 20% error in a body length. They are **not** good enough to quote. Anything a spec is actually authored from should be checked before it goes into that spec's `notes`, because a number that reaches a spec stops being an estimate and starts being documentation that someone else will trust.

This project has already shipped a fabricated citation — a marking recorded in three separate places as measured, in centimetres, to one decimal place, where the figures turned out to be two dimensionless indices lifted from an unrelated source — and a second agent later found the identical trap in a different one. The defence is not better sourcing; it is refusing to state precision that does not exist. So: the only references above are repo paths and line numbers, every one of which was opened and read while writing this file; no figure is stated to more precision than believed; and where a species is marginal or the engine's own data is uncertain, it is said in the row rather than smoothed away.

The one thing here that would normally be a measured number — this biome's share of land — is honestly unknown, and the header says so rather than guessing. The census tool cannot evaluate a slope gate, so bare rock has never been counted; it exists only in the client. Anyone who wants that number will have to measure it in game, and until someone does, no figure should appear in this file.
