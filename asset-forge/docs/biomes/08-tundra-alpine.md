# Tundra / alpine — master species list

**Biome id 8** — `vxc::BiomeId::TUNDRA_ALPINE`, `voxel-core/include/voxelcore/biome.h:32`; mirrored `asset-forge/forge/biomes.py:73`.

| | |
|---|---|
| Gate | MORPHOLOGY, not climate. `classifyBiome` (`biome.h:209-235`) returns this for any column whose surface sits above the temperature-adjusted treeline (`biome.h:221`): 900 m at 0 C mean annual temperature, plus about 150 m per degree C warmer (`biome.h:98-99`). The 900 m base is flagged in that file as TUNED, NOT DERIVED — the only such constant there — and is named as the one to move if the world reads too bare. The per-degree rate is derived from the ~6.5 C/km lapse rate and should not be tuned to compensate. |
| Surface material | Two of them, split on elevation (`biome.h:250-261`): `MAT_PERMAFROST` below `kBiomeAlpineRockLineMm` = 3,200 m, `MAT_ROCK` above it. Every table below says which band an entry belongs to. |
| Share of land (shipped world) | 26.34% on the 121-tile census, second largest biome, with tile `-4_-2` at 100.0% tundra (`docs/measurements/biome-screenshot-targets-2026-08-01.txt:14`). READ THE CORRECTION: the same file re-measured on 289 tiles and got 19.62% (`:94`), and explicitly retracted its own explanation that `elev_gain 1.6` had pushed land above a fixed treeline — "most of that excess was SAMPLE BIAS, not the mechanism I named with confidence" (`:101-106`). Either way it is one of the two or three largest biomes in the world and this list should be built out as if it were. |
| Water present | Yes — melt streams, tarns, and lakes in cirques and behind moraines. A river or lake is classified by the LAND AROUND IT, not by a water biome (`biomes.py:37-42`), so an alpine lake fish is tagged `tundra_alpine`. |
| Asset kinds hosted | `tree`, `bush`, `rock`, `grass`, `reed`, `flower`, `fish`, `cetacean`, `bird` (`biomes.py:74`, the full `_FLIES` set at `biomes.py:50`). `plantable=True`. `cetacean` is hosted only because every plantable biome inherits the same tuple; there is no honest alpine whale and none is listed. |

Two places wearing one id. Below 3,200 m it is permafrost ground: low, dense, wind-flattened vegetation — cushion plants, sedge, dwarf willow lying along the ground rather than standing off it, boulder fields with meltwater between them. Above 3,200 m it is exposed rock, where the only assets are stone, and any living thing in shot is flying or passing through. The list has to serve both, so almost every plant entry names the permafrost band and almost nothing is authored tall: the defining fact of this biome is that it is above the treeline, and an asset that reads as a tree here reads as a bug.

## How to read the tables

**Status** is one of:
* `shipped: <spec-name>` — a spec already exists in `asset-forge/specs/`.
* `queued` — a generator exists for this kind; this is authoring work only.
* `gen: <name>` — needs a generator that does not exist. See the index for the gap list.
* `host: <kind>` — the species is right for the place but the biome's `hosts` tuple in `biomes.py` does not admit that kind yet; that is an engine-side change, not an authoring one.

**Lattice** (animal tables only) is the voxel size the asset should be authored at, by the house rule in `asset-forge/forge/kinds.py:29-58`: a species is drawn at the COARSEST voxel size at which its smallest identifying feature is still about three voxels across. Trees and rocks are not listed with a lattice because they join the world's terrain grid and are 10 cm and nothing else; ground cover, bushes, flowers and reeds are 5 cm.

**⚠** marks a species whose defining feature is at or below what the lattice will hold, explained in the note under its table.

## Trees

This table is short on purpose. The biome is defined as the ground above the treeline, so a full-height tree here is a contradiction — anything woody is either creeping along the ground or is a wind-deformed mat. Dwarf willow, dwarf birch and krummholz are the whole list, and the shipped `tundra-pine` at 9 m is the single tolerated exception, for the ragged transition band right at the line.

| Species | Voxel-artist description | Height or block size (m) | Status |
|---|---|---|---|
| Alpine krummholz | Wind-sheared conifer with no upright leader: a low asymmetric mat of dense dark-green foliage streaming away from the prevailing wind, bare grey deadwood on the windward face, branches touching the ground along their whole length. | 1.8 | `shipped: alpine-krummholz` |
| Tundra pine | Thin open-crowned pine, spire-shaped, sparse whorls with wide gaps between them, grey-brown bark, foliage only in the top third. The transition-band species — permafrost band only, near the treeline. | 9 | `shipped: tundra-pine` |
| Dwarf willow | Prostrate willow, a few centimetres tall and a metre wide: round bright-green leaves in a flat mat over the ground, short reddish twigs, and upright catkins that are the only vertical element. Authored as a ground-hugging tree rather than a bush. | 0.06 tall / 1.0 across | queued |
| Dwarf birch | Low tangled shrub-birch, dense twiggy structure of dark reddish stems with small round scalloped leaves, turning hard orange-red in autumn. Reads as a knee-high thicket, never as a trunk. | 0.8 | queued |

Note on the two dwarfs: at the 10 cm terrain lattice a 6 cm dwarf willow is not a tree, it is one voxel. Author it as a patch — a mat covering several voxels of ground with the catkins standing 2-3 voxels proud — and say so in the spec notes. This is the same fix used for small birds: author to read, and write down that you did.

## Rock types

The richest asset category in the permafrost band and the ONLY one above the rock line. Fifteen shipped rock specs already carry a non-zero `tundra_alpine` weight (`alpine-scree`, `basalt-colonnade`, `cliff-fall-block`, `corestone-tor`, `fault-breccia`, `fractured-outcrop`, `glacial-erratic`, `granite-boulder`, `hero-basalt-colonnade`, `hero-tor-stack`, `jointed-granite-tor`, `karren-pavement`, `standing-stone`, `summit-tor`, `veined-granite`), so this table shows the five that define the look plus the five glacial and periglacial forms that are genuinely missing.

| Species | Voxel-artist description | Height or block size (m) | Status |
|---|---|---|---|
| Alpine scree | Loose angular fragments, flat-faced and sharp-edged, no rounding at all, packed at the angle of repose with the fine material between the coarse. Grey with fresh pale fracture faces. | 1.2 blocks | `shipped: alpine-scree` |
| Summit tor | Stacked residual block pile crowning a ridge, horizontal joint partings between rounded slabs, wider at the base than the top. | 3.2 | `shipped: summit-tor` |
| Jointed granite tor | Rectangular granite blocks separated by two vertical joint sets and one horizontal, giving a masonry look; corners rounded by weathering. | 1.2 | `shipped: jointed-granite-tor` |
| Glacial erratic | A single boulder of the wrong rock, resting on open ground with nothing like it around, part-buried, rounded and slightly flattened on the underside. | 1.2 | `shipped: glacial-erratic` |
| Cliff-fall block | Big freshly detached slab lying where it landed, one clean unweathered fracture face, the others lichened and old; often tilted with a void under one edge. | 2.8 | `shipped: cliff-fall-block` |
| Frost-shattered blockfield | Not a boulder but a FIELD: a level or gently sloping surface entirely covered in angular blocks of local rock, all sizes mixed, no soil visible, edges still sharp because nothing rounds them up here. The single most characteristic ground of the rock band and there is no spec for it. | 0.3-1.5 blocks over a 10-20 m patch | `gen: rock` (blockfield needs a patch-scatter mode, not a single lump) |
| Talus cone | A fan of scree spreading from the mouth of a gully, steep and straight-sided, coarse blocks at the toe and fines at the apex — the reverse of what people expect, and the thing that makes it read as talus. | 8-20 across | `gen: rock` (patch/fan placement) |
| Roche moutonnée | Ice-scoured bedrock whaleback: one smooth, gently sloping, striated face and one abrupt plucked face of ragged steps on the opposite side. The asymmetry IS the asset. | 3-6 | queued |
| Lateral-moraine boulder ridge | Loose unsorted mix, boulders of every size in a fine matrix, heaped along a line with a crest — the material looks dumped rather than fallen or eroded. | 0.5-2.5 blocks | queued |
| Patterned-ground stone stripe | Frost-sorted ground: coarse stones separated into lines or polygon rims with bare fine soil inside them, at metre scale, on nearly flat ground. Reads as a deliberate pattern, which is exactly why it looks strange and right. | 1-3 m cells | `gen: rock` (needs a surface-pattern mode) |

Note: the three `gen: rock` rows are not asking for a new kind. The rock generator makes one lump; a blockfield, a talus cone and patterned ground are DISTRIBUTIONS of lumps over ground. That is a placement/scatter feature, and it should be built once and used by all three rather than faked with three hand-authored mega-boulders.

## Flowers

Permafrost band only. Everything here is short, ground-pressed and disproportionately large-flowered for its size — that ratio is the identifying feature of an alpine flower and the thing to exaggerate.

| Species | Voxel-artist description | Height or block size (m) | Status |
|---|---|---|---|
| Alpine cushion flower | Tight hemispherical cushion of tiny leaves, studded all over with small stalkless flowers so the whole dome reads as one colour block. | 0.24 | `shipped: alpine-cushion-flower` |
| Purple saxifrage | Trailing mat of small dark scale-like leaves under disproportionately large magenta-purple five-petalled flowers held just clear of the mat. Earliest colour in the year — a good early-season palette variant. | 0.05 | queued |
| Arctic poppy | Single hairy stalk, slightly curved, one nodding pale-yellow four-petalled bowl at the tip, no side branches. Silhouette is a stem and a cup, nothing else. | 0.2 | queued |
| Mountain avens | Low woody mat of small crinkled dark leaves with white eight-petalled flowers on short stalks, ageing into a twisted silvery seed head that is a second, distinct look. | 0.12 | queued |
| Edelweiss | Star of thick white woolly bracts lying flat around a cluster of small yellow disc heads, on a short grey-felted stem. The whiteness is felt, not petal — matte, not glossy. | 0.15 | queued |
| Trumpet gentian | A single deep-blue upright trumpet, almost as long as the plant is tall, opening from a flat rosette of leaves at ground level. The most saturated blue in the biome. | 0.08 | queued |

## Ground cover

| Species | Voxel-artist description | Height or block size (m) | Status |
|---|---|---|---|
| Alpine sedge | Stiff narrow blades in a tight tuft, upright rather than arching, dull grey-green, with a few darker seed spikes standing above the blades. | 0.3 | `shipped: alpine-sedge` |
| Tussock cottongrass | Dense tussock of fine blades with tall bare stalks carrying a single white cotton-wool head each; the heads read at distance and the tussock does not. Wet ground beside tarns and melt seeps — authored as `reed`. | 0.5 | queued |
| Reindeer lichen | Pale grey-green branching crust-mat, spongy and coral-like at close range, covering whole flats between boulders. Millimetre-to-centimetre branch structure — never geometry at this project's lattices; the honest asset is a ground material, not an object. | 0.06 | `gen: lichen` |
| Crowberry mat | Dark evergreen creeping mat of tiny needle-like leaves close over the ground, scattered with small black berries. Reads almost as a dark ground texture with relief. | 0.15 | queued |
| Woolly fringe-moss cushion | Grey-green hummock of moss, rounded and slightly lumpy on top, draped over rock edges rather than growing beside them. | 0.1 | queued |

## Bushes / shrubs

Permafrost band only. All of these are pressed down by wind and snow load; none of them should be authored with a clear trunk.

| Species | Voxel-artist description | Height or block size (m) | Status |
|---|---|---|---|
| Juniper scrub | Sprawling low juniper, branches radiating outward and downward from the centre, prickly blue-green foliage in dense clumps, dead grey wood showing through the middle. | 1.1 | `shipped: juniper-scrub` |
| Dwarf mountain pine mat | Multi-stemmed pine lying almost flat, stems curving up only at the tips, dark green needle brushes at the ends of bare branches. Wider than tall by three or four times. | 1.0 tall / 4 across | queued |
| Alpenrose | Rounded dense shrub of small leathery dark leaves with rust-coloured undersides, covered in clusters of deep pink-red bell flowers in season — a strong seasonal palette swap on the same geometry. | 0.8 | queued |
| Arctic willow thicket | Low grey-green thicket of many thin upright twigs, leaves silvery on the underside so the whole mass flickers pale when disturbed, upright fuzzy catkins. | 0.5 | queued |
| Bog bilberry | Small twiggy shrub with rounded blue-green leaves and dull blue berries, growing in patches on damp permafrost ground; reddens hard in autumn. | 0.4 | queued |

## Birds

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| Golden eagle | Large dark-brown raptor with a pale golden nape, long broad wings with deeply slotted primaries held in a shallow V, and a long square tail. | 0.85 | 1 cm | `shipped: golden-eagle` |
| Common raven | All-black heavy corvid, thick deep bill, shaggy throat feathers making the neck look ragged, wedge-shaped tail. | 0.64 | 1 cm | `shipped: common-raven` |
| Rock ptarmigan | Compact round game bird, small head, short bill, feathered legs and feet; white in winter, mottled grey-brown in summer, black tail always. | 0.36 | 1 cm | `shipped: rock-ptarmigan` |
| Bearded vulture | Huge raptor with a long diamond-shaped tail and narrow pointed wings, slate-grey above and rust-orange below, with a dark facial mask and a bristle tuft under the bill. The diamond tail is the identification, not the colour. | 1.15 | 1 cm | queued |
| Snowy owl | Bulky white owl with a round flat-faced head, no ear tufts, yellow eyes, and dark barring that is heavy on females and near-absent on adult males — a clear sex/palette pair on one body. | 0.6 | 1 cm | queued |
| Alpine chough | Glossy black corvid, slimmer than a raven, with a SHORT yellow bill and red legs; broad wings and a long tail, usually in loose noisy flocks. The bill colour is the whole difference from its red-billed relative. | 0.38 | 1 cm | queued |
| Eurasian dotterel | Small plump plover, upright stance, bold white eyebrow meeting in a V at the nape, grey breast band with a white line under it and a rufous belly. Female is the brighter bird, which inverts the usual palette rule. | 0.22 | 1 cm | queued |
| Snow bunting | Small finch-like bird, strikingly white-bodied with black wingtips and back, showing huge white wing flashes in flight; drab buff-and-white when not breeding. | 0.16 | 1 cm | queued |
| White-winged snowfinch | Sparrow-shaped, grey head and brown back, with wings that are mostly WHITE and only show it in flight — a bird that changes appearance completely between perched and flying poses. | 0.17 | 1 cm | queued |
| Horned lark | Ground-hugging lark, sandy-brown above, with a black chest bar, black face mask, yellow throat and two tiny black feather horns on the crown. ⚠ | 0.17 | 1 cm | queued |
| Water pipit | Slim streaked brown bird, long thin bill, white outer tail feathers flashing when it flies, plain grey-brown head; walks rather than hops. | 0.17 | 1 cm | queued |
| Alpine accentor | Sparrow-like but stockier, grey head, rusty-streaked flanks, and a fine dark-tipped yellow-based bill; heavily spotted white throat. | 0.18 | 1 cm | queued |

⚠ **Horned lark**: the "horns" are two feather tufts a few millimetres across. At 1 cm they are one voxel or nothing. Author them above life size — two voxels standing off the crown — and record the exaggeration in the spec `notes`, the same treatment small birds already get. Do NOT try to solve it with a finer lattice; nothing else about the bird needs one.

Palette-not-lattice: the rock ptarmigan's white winter and mottled summer plumage, and the snowy owl's male/female barring, are both material variants on one geometry. Neither is a lattice question and neither should drive a voxel size.

## Land animals

**No generator exists for any of these.** `forge/kinds.py:66-135` has `tree`, `bush`, `rock`, the three tuft kinds, `fish`, `cetacean` and `bird`, and nothing four-legged. Every row below is blocked on `gen: quadruped`, which is a bigger piece of work than a bird was: a quadruped is jointed like a bird but has four limbs in ground contact and a gait, so the generator has to place feet, not just angle wings. The biome hosts the kind conceptually — `hosts` for tundra/alpine is the full `_FLIES` tuple — but there is no `quadruped` key in `kinds.py` for it to admit yet, so unlike bare rock this is purely a generator gap, not a hosts-tuple one.

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| Alpine ibex | Compact stocky goat, forequarters heavier than hind, dun-grey coat with a dark dorsal stripe, and thick backswept horns ridged with transverse knobs curving in a single arc to two-thirds of body length. | 1.5 long / 0.9 shoulder; horn to 1.0 | 5 cm | `gen: quadruped` |
| Chamois | Slender agile goat-antelope, tan body with a black dorsal stripe and a white face split by a bold black stripe from ear to muzzle, and short vertical horns hooked sharply backwards at the very tip. ⚠ | 1.2 / 0.75 | 2 cm | `gen: quadruped` |
| Bighorn sheep | Heavy-bodied brown sheep with a white rump patch and muzzle; the ram's horns are massive, curling a full circle beside the head, thick enough to read as a solid block. | 1.6 / 1.0 | 5 cm | `gen: quadruped` |
| Wild yak | Enormous blackish-brown ox with a high shoulder hump, a skirt of long hair hanging almost to the ground on flanks and legs, and wide low-sweeping horns. The hair skirt is the silhouette. | 3.0 / 1.8 | 5 cm | `gen: quadruped` |
| Reindeer / caribou | Mid-sized deer, grey-brown with a pale neck ruff, broad splayed hooves, and branching antlers with one forward-projecting brow tine over the face — carried by BOTH sexes, which no other deer does. ⚠ | 1.9 / 1.1 | 5 cm | `gen: quadruped` |
| Muskox | Low blocky ox buried in a floor-length dark shaggy coat, short legs barely visible, pale saddle, and horns forming a hard helmet-like boss across the forehead before hooking down and out. | 2.2 / 1.4 | 5 cm | `gen: quadruped` |
| Vicuña | Slight, long-necked, fine-legged camelid, cinnamon above with a white bib of long chest hair and a white underside; no hump, small head, upright ears. The honest WILD high-altitude camelid. | 1.5 / 0.9 | 2 cm | `gen: quadruped` |
| Guanaco | Larger heavier camelid than the vicuña, tawny brown with a grey face and white underparts, long neck held upright, no chest bib. The other honest wild one. | 1.9 / 1.1 | 5 cm | `gen: quadruped` |
| Alpaca | Domesticated camelid: short-legged, deep-bodied, buried in dense uniform fleece that hides the leg joints, with a woolly topknot over the face. Listed because the owner named it, and flagged as what it is — a herded animal, so it belongs beside a settlement, not scattered across wilderness. | 1.5 / 0.9 | 5 cm | `gen: quadruped` |
| Snow leopard | Long-bodied pale grey-white cat with dark rosettes, short muzzle, small round ears, very thick legs, and an enormously thick tail nearly as long as the body. Marginal: genuinely restricted to Central Asian ranges, so it should be a low-weight species, not a generic alpine predator. | 1.1 body / 0.6 shoulder; tail 0.9 | 5 cm | `gen: quadruped` |
| Grey wolf | Deep-chested long-legged canid, straight bushy tail carried low, grizzled grey coat with a pale muzzle and throat; heavier head and shorter ears than any dog silhouette. | 1.2 / 0.8 | 2 cm | `gen: quadruped` |
| Wolverine | Low heavy mustelid built like a small bear, dark brown with a pale band sweeping along each flank and meeting over the rump, bushy tail, huge feet. | 0.85 / 0.4 | 2 cm | `gen: quadruped` |
| Arctic fox | Small short-faced fox with tiny rounded ears and short legs, entirely white and enormously fluffy in winter, grey-brown and lean in summer — two very different silhouettes on one skeleton. | 0.55 / 0.3 | 2 cm | `gen: quadruped` |
| Mountain hare | Long-eared hare with black ear tips, powerful hind legs far larger than the front, white in winter and blue-grey in summer. The black ear tips stay black in every season and are the one constant mark. | 0.55 / 0.25 | 2 cm | `gen: quadruped` |
| Alpine marmot | Fat low-slung ground squirrel the size of a cat, grey-brown, small ears, short bushy tail, sitting upright on the hind legs as its characteristic pose. Author the upright pose — it is how the animal is recognised. | 0.5 / 0.2 | 2 cm | `gen: quadruped` |
| Pika | Round tail-less ball of grey-brown fur with large round ears and no visible neck; looks like a hamster, not a rabbit. ⚠ | 0.18 | 1 cm | `gen: quadruped` |
| Arctic ground squirrel | Stocky ground squirrel, tawny with pale spotting on the back, short furred tail, sits upright at burrow mouths. | 0.3 | 1 cm | `gen: quadruped` |
| Norway lemming | Tiny compact rodent with a bold black-and-yellow-brown patterned coat, blunt face, ears hidden in fur, almost no tail. The pattern is the whole read. ⚠ | 0.13 | 1 cm | `gen: quadruped` |
| Alpine bumblebee | Furry black-and-yellow-banded body with a pale tail, wings blurred in flight. ⚠ | 0.02 | 1 cm | `gen: arthropod` |
| Apollo butterfly | Large white butterfly with translucent-edged wings and two red black-ringed eyespots on each hindwing. ⚠ | 0.08 wingspan | 1 cm | `gen: arthropod` |

⚠ notes, and the distinction worth carrying out of this section:

* **Horns survive a coarse lattice; antler tines do not.** An ibex is identified by its horns and effectively nothing else, and a horn is a thick continuous arc — 8-10 cm at the base, tapering — so at 5 cm it is still two to three voxels wide over a 20-voxel sweep and reads correctly. The transverse knobs on it are ~4 cm and will vanish; that is fine, because the silhouette carries the species without them. A **reindeer** antler is the opposite case: the identifying brow tine is 3-4 cm thick, under one voxel at 5 cm, so at that lattice the animal loses the only feature that separates it from every other deer. Author the antlers thickened above life size and say so, or accept that it reads as a generic deer.
* **Chamois**: the horn is thin (~3 cm) and short, and the hook at the tip is the identification. 2 cm gets the hook to about 1.5 voxels — still marginal. Thicken the horn deliberately.
* **Pika** at 18 cm is near the floor of what any lattice holds as an animal rather than a lump: at 1 cm the body is 18 voxels, which is fine, but the round ears are ~2 cm and land on two voxels. It works, barely, and only at 1 cm.
* **Norway lemming** at 13 cm: the body is holdable at 1 cm but the black-and-tan pattern is the identification, and pattern is a palette problem, not a lattice one — so this one is actually fine as long as the material work is done.
* **Bumblebee and apollo butterfly**: neither is real geometry at 1 cm — a bee is two voxels of body and a butterfly's wing is a single voxel plate. If insects are wanted, they must be authored well above life size as a deliberate readability choice, recorded in the spec notes, exactly as small birds already are. Do not add a 2 mm lattice for them.

Palette-not-lattice, again: the arctic fox and mountain hare seasonal coats and the snow leopard's rosettes are all material variants.

## Fish

Cold, clear, oxygen-rich water with very little in it. A short table is the correct answer — alpine tarns are species-poor, and that is a feature of the place, not a gap in the list.

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| Brown trout | Torpedo-shaped, olive-brown above fading to cream below, covered in black and red spots each ringed with pale haloes; small adipose fin behind the dorsal. | 0.30 | 1 cm | `shipped: brown-trout` |
| Arctic char | Slimmer than a trout, dark blue-green back with PALE spots on a dark ground — the inverse of a trout's dark-on-pale — and brilliant orange-red belly and white-edged lower fins in spawning condition. | 0.45 | 2 cm | queued |
| European minnow | Small slender shoaling fish, olive with a broken dark side stripe and a pale belly; blunt-nosed. ⚠ | 0.09 | 1 cm | queued |

⚠ **European minnow** at 9 cm is 9 voxels long at 1 cm, which is below the 20-90 voxel range `kinds.py:129-134` says birds are drawn to read at. Either author it above life size or drop it; a 9-voxel fish is a dash, not a species. Note also that it is a lowland-stream fish that reaches only the lower alpine valleys — marginal for this biome and it should carry a low weight if it ships at all.

## Build priority

1. **Frost-shattered blockfield and talus cone.** The rock band above 3,200 m currently has boulders and nothing else, and boulders scattered on empty rock read as props. Blockfield is the ground of that entire band. Both need the same patch-scatter feature, so it is one piece of work covering two of the most visible assets in the second-largest biome.
2. **Dwarf willow, dwarf birch and the dwarf mountain pine mat.** The permafrost band has exactly two shipped trees and one shipped bush. Three prostrate woody assets, all using the existing tree/bush machinery with no new generator, would change the read of every below-3,200 m scene.
3. **The four remaining flowers and the cottongrass.** The tuft generator already exists in three settings; these are authoring-only and alpine flowers are the cheapest colour in a biome that is otherwise grey, white and dun.
4. **Bearded vulture, alpine chough, snow bunting.** Birds are the only animate thing that can be in an alpine shot today, the generator is mature, and a soaring diamond-tailed vulture over a rock face is the single most evocative asset on this list per hour spent.
5. **`gen: quadruped`.** Twenty entries on this list are blocked on it, and it is the largest single unlock in the file — but it is a new jointed generator with a gait problem, so it should be scheduled as its own project, not slipped into a biome pass. Ibex and chamois are the right first two species: horns survive the lattice, so the first quadruped ever built will actually read as its species.

## Where the numbers come from

Every size in this file is an **approximate typical adult figure from general knowledge**. Nothing here is measured, and nothing here is sourced — there is no dataset, paper or survey behind any number in these tables, and none is cited because none exists.

They are good enough for the one job they have: choosing a lattice, which is a decision about whether a feature lands on one voxel or three, and is not sensitive to a 20% error in a body length. They are **not** good enough to quote. Anything a spec is actually authored from should be checked before it is written into that spec's `notes`, because a number that reaches a spec stops being an estimate and starts being documentation.

This matters here specifically. This project has already shipped a fabricated citation — a marking documented in three places as measured, in centimetres, to one decimal, where the figures turned out to be dimensionless indices lifted from an unrelated source — and a second agent later found the same trap in a different one. The defence is not better sourcing, it is refusing to state precision that does not exist. So: no citations above except repo paths and line numbers, all of which were opened and read while writing this file; no number stated to more precision than believed; and the marginal species (snow leopard, alpaca, European minnow) flagged in their own rows rather than smoothed into the list.

The one number in this file that IS measured is the biome's share of land, and it is measured twice with different answers — 26.34% on 121 tiles, 19.62% on 289 — with the census file retracting its own explanation of the difference (`docs/measurements/biome-screenshot-targets-2026-08-01.txt:101-106`). Both figures are in the header row above, deliberately, because quoting only the first would repeat the error that file was written to correct.
