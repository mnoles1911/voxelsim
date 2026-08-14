# Temperate forest — master species list

**Biome id 3** — `vxc::BiomeId::TEMPERATE_FOREST`, `voxel-core/include/voxelcore/biome.h:27`;
mirrored `asset-forge/forge/biomes.py:63-64`.

| | |
|---|---|
| Climate envelope | Two ways in (`biome.h:233-234`): **moderate** — 800–1600 mm/yr, failing either the 18 °C warm test or the 70% precipitation-seasonality test; or **wet and not warm** — over 1600 mm/yr with a mean annual temperature under 18 °C. It is the default forested band: what is left after taiga takes the cold, rainforest takes the wet-and-warm, and savanna takes the seasonal. |
| Surface material | topsoil (`MAT_TOPSOIL`, `biome.h:244`) |
| Share of land (shipped world) | **4.90%** over 289 tiles, best tile `-8_-12` at 97.2% (`docs/measurements/biome-screenshot-targets-2026-08-01.txt`) |
| Water present | yes — streams, rivers, ponds; this is the biome with the most riparian corridor per unit area |
| Asset kinds hosted | everything: `tree`, `bush`, `rock`, `grass`, `reed`, `flower`, `fish`, `cetacean`, `bird` (`biomes.py:64`, `_FLIES`) |

**Read the share number carefully, because the obvious story about it is
retracted in its own source.** An earlier 121-tile pass measured temperate forest
at 2.43% and blamed savanna for taking the warm-seasonal band ahead of it in
`classifyBiome`'s gate order. On 289 tiles it is 4.90%, and that file's
CORRECTION 2 says plainly: a real gate-order effect exists, but the number the
argument was built on was small-sample. So temperate forest is genuinely
under-represented against Earth's ~10–15%, and the *size* of that shortfall is
smaller than it was first written down as. Do not repeat the 2.43% figure.

The important thing for an asset list is not the share but the structure. This is
the only biome in the world with a real **canopy, understorey and forest floor**
as three distinct layers, and the species list has to fill all three. A player
standing in it sees very few trees at once and a great deal of ground.

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

The deepest shipped tree set of any biome — 11 of 17 specs carry a temperate
forest weight — and still the thinnest relative to what a real deciduous forest
holds. The queued list below is chosen for **crown shape and bark**, because in
a closed canopy those are the only things visible.

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Pedunculate oak | Massive short bole, heavy low limbs, broad dome wider than tall | 14 | `shipped: temperate-oak` |
| Silver birch | Slim white trunk with black diamond scars, fine drooping twigs, light open crown | 16 | `shipped: birch` |
| Field elm | Tall vase crown, branches rising then arching | 11 | `shipped: field-elm` |
| River broadleaf | Wide low crown leaning over water, heavy lower limbs | 12 | `shipped: river-broadleaf` |
| Weeping willow | Curtain of pendulous shoots to the ground | 12 | `shipped: weeping-willow` |
| Cherry blossom | Small spreading crown, horizontal banded bark, dense pale bloom | 6.5 | `shipped: cherry-blossom` |
| Columnar cypress | Narrow dark flame, no visible trunk | 13 | `shipped: columnar-cypress` |
| Hawthorn | Low dense thorny irregular crown | 3.8 | `shipped: hawthorn-scrub` |
| Pine | Straight bare trunk, whorled branches, dark conical crown | 9 | `shipped: tundra-pine` |
| Giant sequoia | Enormous fluted red-brown trunk, buttressed base, crown only in the top third | 80 | `shipped: hero-sequoia` |
| Sapling | Slim stem, small sparse crown | 4.5 | `shipped: temperate-sapling` |
| European beech | Smooth pewter-grey trunk with no fissures at all, a very dense crown that darkens the ground beneath it, and almost nothing growing under it | 30 | `queued` |
| Hornbeam | Fluted, muscled, sinewy grey trunk — the bole looks twisted under tension; dense low crown | 20 | `queued` |
| Sweet chestnut | Thick trunk with bark fissures spiralling around it, long coarse-toothed leaves | 25 | `queued` |
| Sycamore maple | Heavy dome crown, flaking plate bark, hanging seed keys | 25 | `queued` |
| Sugar maple | Symmetrical broad oval crown; the identifying feature is autumn colour, which is a palette variant rather than geometry | 25 | `queued` |
| Small-leaved lime | Straight trunk with a dense burr mass of shoots around the base, tall domed crown | 25 | `queued` |
| Common alder | Multi-stemmed, dark, standing in water with visible arched roots and small black cones held on bare twigs | 18 | `queued` |
| Wych elm | Broad low fan crown from a short trunk that forks near the ground | 20 | `queued` |
| European yew | Enormously thick fluted trunk with many fused stems, dark, low, wider than tall, hollow at the centre | 12 | `queued` |
| Holly | Dense dark evergreen cone in the understorey, glossy spined leaves, red berries | 8 | `queued` |
| Douglas fir | Very tall straight spire, deeply corky bark, drooping outer branchlets | 50 | `queued` |
| Western hemlock | Tall conifer whose leader **droops over at the tip** — the drooping top is the identifying feature at any distance | 45 | `queued` |
| Sitka spruce | Rigid narrow spire, whorled stiff branches, blue-grey cast | 45 | `queued` |
| Western red cedar | Buttressed fluted base, stringy shredding red-brown bark, drooping frond sprays | 50 | `queued` |
| Bigleaf maple | Heavy spreading limbs carrying thick moss and fern mats — the epiphyte load is the species | 25 | `queued` |
| Tulip tree | Very straight tall trunk, high crown, distinctly square-cut leaf silhouette | 35 | `queued` |
| American beech | Smooth grey trunk, low retained dead leaves through winter, dense understorey suckers | 25 | `queued` |
| Eastern hemlock | Fine-textured, dark, drooping, deeply shading a streamside slope | 30 | `queued` |
| Shagbark hickory | The bark is the species: long curling plates peeling away from the trunk at both ends | 25 | `queued` |
| Black cherry | Dark scaly "burnt cornflake" bark, narrow crown, drooping white flower racemes | 20 | `queued` |
| Flowering dogwood | Small understorey tree, tiered horizontal branching, large white bracts | 8 | `queued` |
| Japanese maple | Very small, wide, layered, with a fine twig structure and deep red foliage | 5 | `queued` |
| Rowan | Feathery pinnate leaves and orange-red berry clusters | 8 | `queued` |
| Fallen mossy log | A whole horizontal trunk on the floor with a root plate at one end, buried in moss and carrying seedlings — a nurse log. Not a species; the single most characteristic *object* on a temperate rainforest floor | 12 long | `queued` — closest is `desert-dead` laid down |

## Rock types

Forest rocks are all about what grows on them: a bare boulder reads as wrong in a
closed canopy, and the shipped `mossy-forest-boulder` is the model.

| Rock | Voxel-artist description | Block size (m) | Status |
|---|---|---|---|
| Mossy forest boulder | Rounded block with moss on the upper surfaces only, bare where water runs | 1.2 | `shipped: mossy-forest-boulder` |
| Granite boulder | Rounded joint block with three families of flat faces under the rounding | 1.6 | `shipped: granite-boulder` |
| Glacial erratic | Out-of-place block sitting on the surface, part-buried | 1.2 | `shipped: glacial-erratic` |
| Cliff-fall block | Large fresh angular block with sharp arrises where it landed | 2.8 | `shipped: cliff-fall-block` |
| Veined granite | Pale rock cut at an angle by darker or quartz-white veins | 1.2 | `shipped: veined-granite` |
| Exfoliating dome | Swelling dome shedding curved shells | 1.2 | `shipped: exfoliating-dome` |
| Limestone pinnacles | Sharp fluted spires clustered together | 0.9 | `shipped: limestone-pinnacles` |
| River cobble | Rounded flattened water-worn stone | 1.2 | `shipped: river-cobble` |
| Root-split block | A boulder with a tree root wedged through a joint and prising it open — the root is the story and it needs geometry from the tree side | 1.4 | `queued` ⚠ |
| Leaf-littered slab | Flat low bedrock plate half-buried in leaf litter, only its high edge showing | 1.8 | `queued` |
| Streambed cascade block | Angular blocks wedged in a channel with water-polished tops and dark wet flanks | 1.0 | `queued` |
| Mossy sandstone bench | Bedded step with a thick continuous moss carpet over the top and ferns on the riser | 2.0 | `queued` |
| Tufa / travertine curtain | A pale porous drapery built up where a spring runs over a lip, with a rounded lobed front | 1.5 | `queued` |
| Sinkhole rim | A collapse ring in limestone with an overhanging lip and a dark void | 3.0 | `queued` |
| Charcoal-blackened block | A stone from an old burn, sooted on one side only | 1.2 | `queued` — palette variant |

⚠ **Root-split block.** The rock generator makes rocks and the tree generator
makes roots, and this asset needs both in one grid. It is listed because it is a
genuinely characteristic forest object and because it is the clearest example in
this document of a *composite* asset — two generators' output in one spec — which
nothing in `forge/pipeline.py` currently does. Treat it as a question for the
kind-list owner, not as authoring work.

## Flowers

The forest floor's spring flora is the strongest visual event in the biome and it
has one generic spec today.

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Meadow daisy | Leaf rosette with a few white-and-yellow disc blooms | 0.42 | `shipped: meadow-daisy` |
| Understorey flower | Generic tall shade-plant bloom | 0.75 | `shipped: jungle-understory-flower` |
| Common bluebell | Dense carpet: one arched stalk per plant carrying a one-sided row of nodding deep violet bells. **The carpet is the asset** — a single bluebell is nothing and a hectare of them is the most memorable thing in the biome | 0.35 | `queued` |
| Wood anemone | Low, one white six-petal star per stem over a whorl of three divided leaves, closing at dusk | 0.15 | `queued` |
| Ramsons / wild garlic | Broad flat bright green lance leaves in a dense sheet with white star-burst heads on triangular stalks | 0.4 | `queued` |
| Foxglove | A single tall one-sided spike of large drooping pink-purple thimbles, spotted inside; a clearing and track-edge plant | 1.5 | `queued` |
| Primrose | Tight rosette of crinkled leaves with pale yellow flowers on short individual stalks straight from the centre | 0.15 | `queued` |
| Wood sorrel | Very low, three heart-shaped leaflets folding down at night, one small white flower with lilac veins | 0.1 | `queued` ⚠ |
| Lily of the valley | Two broad upright leaves and one arched stem of small white bells behind them | 0.25 | `queued` |
| Common dog violet | Very low heart-shaped leaves with small blue-violet flowers held just above | 0.12 | `queued` ⚠ |
| Red campion | Branched hairy stem with flat pink five-petal flowers, each deeply notched | 0.7 | `queued` |
| Herb robert | Sprawling red-stemmed plant with finely cut leaves and small hard-pink flowers | 0.35 | `queued` |
| Large-flowered trillium | Three leaves, three petals, one flower, all in one whorl — the cleanest three-fold silhouette available | 0.35 | `queued` |
| Trout lily | Two mottled brown-green leaves and one nodding yellow flower with strongly reflexed petals | 0.2 | `queued` |
| Hellebore | Coarse dark palmate evergreen leaves under nodding green-cream cups, flowering in winter | 0.5 | `queued` |
| Cyclamen | Round marbled leaves flat on the ground with pink flowers on bare stems whose petals point straight up | 0.15 | `queued` |
| Bugle | Low creeping mat sending up short dense spikes of blue flowers with bronze-purple leaves | 0.25 | `queued` |

⚠ **Wood sorrel and dog violet.** Both are 10–12 cm plants with 1–2 cm flowers,
on a 5 cm ground-cover lattice. The flower is well under one voxel. The tuft
generator's existing answer is to draw the bloom as a single voxel in a distinct
colour, which is what `meadow-daisy` does and which does read — but it means the
flower is drawn at three to five times life size. That is fine and it is already
house practice; write it in the spec's `notes` so nobody "fixes" it.

## Ground cover

The forest floor is mosses and ferns, not grass, and neither is well served.
Reeds are their own kind (`biomes.py:35`) sharing the tuft generator.

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Jungle groundcover | Generic broad-leaved shade floor plant | 0.8 | `shipped: jungle-groundcover` |
| Meadow grass | Soft tuft of fine blades, for glades and rides | 0.45 | `shipped: meadow-grass` |
| Bulrush | Stiff stems with dark brown cigar heads | 1.4 | `shipped: bulrush` |
| Common reed | Tall canes with a purple-brown plume | 2.0 | `shipped: water-reed` |
| Bracken | Coarse triangular fronds each on a single bare stalk, in a uniform waist-high stand that excludes everything else | 1.5 | `queued` |
| Lady fern | Delicate finely divided fronds in a symmetrical shuttlecock rosette | 0.9 | `queued` |
| Hart's tongue fern | The odd one out: undivided glossy strap fronds in a rosette, with a crinkled edge | 0.5 | `queued` |
| Sword fern | Stiff dark leathery once-divided fronds in a dense upright rosette; the Pacific forest floor | 1.2 | `queued` |
| Male fern | Big coarse shuttlecock of twice-divided fronds with scaly stalk bases | 1.2 | `queued` |
| Sphagnum moss | A soft pale-green to rust-red hummock, saturated, with no visible individual stems | 0.15 | `queued` |
| Hair-cap moss | Upright dark green stems in a dense turf, each with a fine star of leaves; a bristly texture rather than a cushion | 0.1 | `queued` |
| Feather moss carpet | A continuous fine gold-green mat over the ground and over fallen wood — a *surface*, and the honest question is whether it should be a material rather than an asset | 0.05 | `queued` ⚠ |
| Ivy ground layer | Dark glossy lobed leaves in a flat sheet, climbing anything vertical it meets | 0.15 | `queued` |
| Wood sedge | Loose arching bright green blades with drooping thin seed spikes | 0.5 | `queued` |
| Dog's mercury | Dull dark green upright unbranched stems in a uniform sheet, no visible flowers | 0.35 | `queued` |
| Bilberry mat | Low woody-stemmed dense green mat with small round leaves and dark berries | 0.4 | `queued` |
| Wood horsetail | Whorls of fine drooping branchlets in tiers up a hollow stem — a miniature conifer | 0.5 | `queued` |

⚠ **Feather moss carpet.** A continuous mat 2–5 cm thick on a 5 cm lattice is one
voxel. That is not an object; it is the top of the ground. The honest
implementation is a **surface material** on the terrain, the way
`biomeSurfaceMaterial` already paints topsoil, not a tuft asset — and this is
worth settling before anyone authors it, because a one-voxel mat scattered as
individual entities costs entity count for something a material gives free.

## Bushes / shrubs

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Bramble thicket | Dense arching thorny canes rooting where they touch | 1.5 | `shipped: bramble-thicket` |
| Hazel coppice | Many straight poles rising from one stool at ground level, broad round leaves, no central trunk | 5 | `queued` |
| Holly understorey | Dense dark evergreen cone with glossy spined leaves | 3 | `queued` |
| Elder | Loose open shrub with soft pithy stems, flat white flower plates then heavy black berry umbels | 4 | `queued` |
| Common dogwood | Upright stems that go blood-red in winter — the winter colour is the species | 3 | `queued` |
| Spindle | Slim green four-angled twigs, pink four-lobed fruit splitting to orange | 3 | `queued` |
| Guelder rose | Maple-shaped leaves, flat white flower heads, translucent red berries | 3 | `queued` |
| Rhododendron thicket | Very dense dark evergreen mound of leathery leaves with heavy purple trusses; forms an impenetrable understorey | 4 | `queued` |
| Salal | Low dense glossy oval-leaved evergreen mat under conifers | 1.2 | `queued` |
| Red huckleberry | Fine green-twigged airy shrub, very open, with small bright red berries | 2 | `queued` |
| Witch hazel | Spreading zigzag branches with spidery yellow ribbon flowers on bare wood | 4 | `queued` |
| Mountain laurel | Dense rounded evergreen with gnarled twisted stems and clusters of cup flowers | 3 | `queued` |
| Yew (shrub form) | Dark, dense, spreading low, with fine flat needles and red berry cups | 2.5 | `queued` |
| Butcher's broom | Stiff dark spine-tipped flattened stems that look like leaves, with a berry sitting in the middle of each | 0.8 | `queued` |

## Birds

Thirteen shipped, more than any biome but grassland. The queued list targets what
the shipped set lacks: the small canopy birds that carry a *pose* (creeping up a
trunk, hanging upside down) and the large forest raptors and game birds.

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Tawny owl | Round-headed, no ear tufts, mottled brown, black eyes, upright on a branch | 0.40 | 1 cm | `shipped: tawny-owl` |
| Great spotted woodpecker | Pied black-and-white with a bold white shoulder oval, red under the tail, clinging vertically | 0.23 | 1 cm | `shipped: great-spotted-woodpecker` |
| Eurasian jay | Pink body, black moustache, white rump, blue barred wing patch | 0.34 | 1 cm | `shipped: eurasian-jay` |
| Common buzzard | Broad wings, short fanned tail, pale chest crescent | 0.52 | 1 cm | `shipped: common-buzzard` |
| Common raven | Big, wedge tail, heavy bill | 0.64 | 1 cm | `shipped: common-raven` |
| Song thrush | Warm brown above, arrow-spotted below | 0.23 | 1 cm | `shipped: song-thrush` |
| European robin | Round, upright, orange face and breast. **Authored at 24 cm against a real 14 cm** | 0.24 | 1 cm | `shipped: european-robin` |
| Great tit | Black head, white cheeks, yellow with a black belly stripe. **Authored at 24 cm against a real 14 cm** | 0.24 | 1 cm | `shipped: great-tit` |
| Common starling | Compact, short-tailed, spangled | 0.21 | 1 cm | `shipped: common-starling` |
| Barn swallow | Forked tail streamers, red throat. **Authored at 26 cm against a real 17–19 cm** | 0.26 | 1 cm | `shipped: barn-swallow` |
| Common kingfisher | Dagger bill, electric blue and orange. **Authored at 20 cm against a real 17 cm** | 0.20 | 1 cm | `shipped: common-kingfisher` |
| Grey heron | Long S-neck, dagger bill, crest plume | 1.00 | 1 cm | `shipped: grey-heron` |
| Mallard | Green head, white collar | 0.58 | 1 cm | `shipped: mallard-duck` |
| Eurasian nuthatch | Blue-grey above, buff below, black eye stripe, no visible neck, and the only bird that goes **head-first down** a trunk — the pose is the species | 0.14 | 1 cm | `queued` ⚠ authored-up |
| Eurasian treecreeper | Tiny, mottled brown, curved fine bill, stiff tail pressed to the bark, spiralling up a trunk | 0.13 | 1 cm | `queued` ⚠ authored-up |
| Goldcrest | The smallest bird here at 9 cm, olive-green with a bright yellow-orange crown stripe. **Below every workable size**; see the note | 0.09 | 1 cm | `queued` ⚠ |
| Common blackbird | All-black male with an orange-yellow bill and eye ring; the bill is the only mark and it must carry alone | 0.25 | 1 cm | `queued` |
| Common chaffinch | Grey-blue crown, pink face and breast, and two hard white wing bars on black | 0.15 | 1 cm | `queued` ⚠ authored-up |
| Eurasian bullfinch | Fat, short-billed, black cap, brilliant rose-red breast, bold white rump in flight | 0.16 | 1 cm | `queued` ⚠ authored-up |
| Blue tit | Blue cap, white face with a dark eye line, yellow below; usually drawn hanging upside down | 0.12 | 1 cm | `queued` ⚠ authored-up |
| Long-tailed tit | A ball of feathers with a tail longer than the body — the proportion is the entire species | 0.14 | 1 cm | `queued` ⚠ |
| Eurasian wren | Tiny, round, rusty brown, with a short tail held **cocked vertically** | 0.10 | 1 cm | `queued` ⚠ |
| European green woodpecker | Large, green-backed, red crown, yellow rump, and mostly found on the ground on anthills | 0.33 | 1 cm | `queued` |
| Black woodpecker | Crow-sized, all matt black with a red crown patch, very long neck for a woodpecker | 0.47 | 1 cm | `queued` |
| Eurasian sparrowhawk | Short broad wings and a long square tail — the exact opposite proportion to a falcon | 0.35 | 1 cm | `queued` |
| Northern goshawk | Bigger, heavier, with a bold white eyebrow and a barred grey breast | 0.55 | 1 cm | `queued` |
| Eurasian woodcock | Fat body, very long straight bill held down, huge eyes set far back, cryptic dead-leaf plumage | 0.34 | 1 cm | `queued` |
| Common pheasant | Long pointed tail half the total length, red face wattle, white neck ring | 0.80 | 1 cm | `queued` |
| Wild turkey | Very large, iridescent bronze-black, bare blue-and-red head, fanned tail in display | 1.10 | 1 cm | `queued` |
| Pileated woodpecker | Crow-sized, black with a flaming red triangular crest and white neck stripes | 0.45 | 1 cm | `queued` |
| Wood thrush | Warm rusty head, white breast with large round black spots | 0.20 | 1 cm | `queued` ⚠ authored-up |
| Scarlet tanager | The male is unbroken brilliant scarlet with solid black wings and tail — two flat colours, no pattern, and it works | 0.17 | 1 cm | `queued` ⚠ authored-up |
| Common cuckoo | Grey, slim, long-tailed, with pointed wings; looks like a sparrowhawk in flight and that resemblance is the point | 0.33 | 1 cm | `queued` |
| Common redstart | Slate grey above, orange below, with a **constantly quivering rusty-orange tail** | 0.14 | 1 cm | `queued` ⚠ authored-up |

⚠ **The small-passerine floor, and one species below it.**

Nine rows above are 12–20 cm, which at 1 cm is 12–20 voxels. The library already
authored four birds at 20–26 cm against real lengths of 14–19 cm, each recording
the reason in its own `notes`, because a perched songbird sits at 36–42° nose-up
and loses length to the projection. Every flagged row needs the same treatment,
and the reason must go in the spec.

**The goldcrest is different and should probably not be built.** At 9 cm it is
nine voxels — smaller than Minecraft's shipped parrot at 11 — and `kinds.py:130`
states the bird generator is drawn to read at 20–90 voxels. Authoring it up to
20 cm makes it more than twice life size, at which point it is no longer a
goldcrest sitting next to a robin, it is a robin. Either accept that and say so,
or leave it out. The same reasoning applies more weakly to the wren at 10 cm.

**Long-tailed tit** is flagged for the opposite reason: its identity is a *ratio*
(tail longer than body), and a ratio survives any lattice. It is one of the
cheapest legible species in this whole document and should be built early.

## Land animals

The richest land-animal biome in the temperate world, and every row is blocked on
a generator that does not exist.

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| Red deer stag | Long-legged deer with a maned neck and branched antlers spreading nearly a metre. **The antlers are the whole silhouette and no lattice holds them at life thickness** | 2.0 / 1.2 sh | 2 cm | `gen: quadruped` ⚠ |
| Red deer hind | The same animal without antlers and with a slimmer neck | 1.9 / 1.1 sh | 5 cm | `gen: quadruped` |
| Roe deer | Small, short-bodied, high-rumped, bright white rump patch, short three-point antlers | 1.2 / 0.7 sh | 2 cm | `gen: quadruped` |
| Fallow deer | Boldly white-spotted on chestnut, with **palmate** antlers — flat 10–15 cm blades that survive a coarse lattice where round tines do not | 1.5 / 0.95 sh | 5 cm | `gen: quadruped` |
| Sika deer | Compact dark deer with a white rump bordered black and a distinct white spot pattern in summer only | 1.4 / 0.85 sh | 5 cm | `gen: quadruped` |
| White-tailed deer | Slim, tan, with a broad tail held straight up showing pure white underneath when it flees — that raised tail is the species | 1.8 / 1.0 sh | 5 cm | `gen: quadruped` |
| Elk / wapiti | Much larger than a red deer, pale rump patch, dark neck mane, very long sweeping antlers | 2.4 / 1.5 sh | 5 cm | `gen: quadruped` ⚠ |
| Moose | The largest: humped shoulder, very long legs, a pendulous bell of skin under the throat, drooping muzzle, and broad palmate antlers | 2.8 / 1.9 sh | 5 cm | `gen: quadruped` |
| Wild boar | Wedge body heaviest at the shoulder, head carried low, coarse dark bristle, small tusks | 1.4 / 0.8 sh | 5 cm | `gen: quadruped` |
| Brown bear | Massive, with a pronounced shoulder hump, a dished face profile, and a very short tail; walks flat-footed | 2.0 / 1.1 sh | 5 cm | `gen: quadruped` |
| American black bear | Smaller, **no shoulder hump**, a straight face profile, taller ears — the hump and the profile are the only separators | 1.6 / 0.9 sh | 5 cm | `gen: quadruped` |
| Grey wolf | Deep narrow chest, long legs, straight bushy tail carried low | 1.3 / 0.8 sh | 2 cm | `gen: quadruped` |
| Eurasian lynx | Short body, very long legs, a short black-tipped stump tail, and **black ear tufts with a flared cheek ruff** | 1.0 / 0.65 sh | 2 cm | `gen: quadruped` ⚠ |
| Bobcat | Smaller lynx with a barred tail and less dramatic tufts | 0.8 / 0.5 sh | 1 cm | `gen: quadruped` ⚠ |
| European wildcat | Heavier than a house cat with a blunt thick ringed black-tipped tail | 0.6 / 0.35 sh | 1 cm | `gen: quadruped` ⚠ |
| Red fox | Slender, black-backed pointed ears, white-tipped brush | 0.7 / 0.4 sh | 2 cm | `gen: quadruped` |
| European badger | Very low and wide, wedge-shaped, black-and-white striped face | 0.8 / 0.3 sh | 2 cm | `gen: quadruped` |
| Pine marten | Long slender arboreal mustelid with a very bushy tail and a cream-yellow throat bib | 0.5 (0.75 total) | 1 cm | `gen: quadruped` |
| Fisher | Larger, darker, heavier-headed marten with a long tapering tail | 0.6 (1.0 total) | 1 cm | `gen: quadruped` |
| Red squirrel | Small, rust-red, with **ear tufts** and a long fully-plumed tail carried in an S over the back. The tail is the whole silhouette | 0.22 (0.42 total) | 1 cm | `gen: quadruped` ⚠ |
| Eastern grey squirrel | Larger, grey, no ear tufts, a broader flatter tail held as a fan | 0.26 (0.5 total) | 1 cm | `gen: quadruped` ⚠ |
| Eastern chipmunk | Small striped ground squirrel: five dark stripes down the back, bulging cheek pouches, short tail held up | 0.15 (0.25 total) | 1 cm | `gen: quadruped` ⚠ |
| Hazel dormouse | Golden, huge black eyes, and a fully furred tail — the furred tail separates it from every mouse | 0.08 | — | out of scope ⚠ |
| European otter | Long low sinuous body, thick tapering tail, short legs, broad flat muzzle | 0.8 (1.2 total) | 2 cm | `gen: quadruped` |
| Eurasian beaver | Heavy body, small head, and a flat scaly paddle tail as wide as the body | 0.9 (1.2 total) | 2 cm | `gen: quadruped` |
| European polecat | Long low mustelid with a dark bandit face mask over pale underfur | 0.45 | 1 cm | `gen: quadruped` |
| Stoat | Very long and thin, chestnut over cream, with a **black tail tip** that stays black in winter white | 0.3 | 1 cm | `gen: quadruped` ⚠ |
| American mink | Uniform dark chocolate, slightly smaller and slimmer than a polecat, with a small white chin patch | 0.4 | 1 cm | `gen: quadruped` |
| Raccoon | Low hunched body, black eye mask, ringed tail | 0.5 / 0.3 sh | 2 cm | `gen: quadruped` ⚠ |
| Striped skunk | Low, fluffy, black with a white cap splitting into two stripes down the back, and a huge plume tail | 0.4 (0.7 total) | 2 cm | `gen: quadruped` |
| Virginia opossum | Pale grizzled grey with a pointed pink snout, bare black ears, and a long naked prehensile tail | 0.45 (0.8 total) | 2 cm | `gen: quadruped` |
| North American porcupine | A hunched dark mass covered in raised quills with a short thick tail; the quills are a surface, not geometry | 0.7 | 2 cm | `gen: quadruped` |
| European hedgehog | Spine-covered dome with a small pointed face | 0.25 | 1 cm | `gen: quadruped` |
| European hare | Very long black-tipped ears, long hind legs | 0.65 / 0.25 sh | 1 cm | `gen: quadruped` ⚠ |
| European rabbit | Shorter ears, rounder body, white tail flash | 0.4 / 0.2 sh | 1 cm | `gen: quadruped` |
| Fire salamander | Glossy black with irregular bright yellow blotches, short legs, blunt head, slow | 0.2 | 1 cm | `gen: quadruped` |
| Common frog | Squat, folded hind legs, bulging eyes on top of the head; the crouched pose is the whole read | 0.09 | 1 cm | `gen: quadruped` ⚠ |
| Grass snake | Olive-green with a yellow-and-black collar, smooth taper head to tail | 1.0 | 1 cm | `gen: serpentine` |
| European adder | Shorter, thicker, hard black zigzag down the spine | 0.6 | 1 cm | `gen: serpentine` ⚠ |
| Slow worm | A legless lizard: a bronze cylinder with no neck, no pattern, and a blunt tail — nearly featureless by design | 0.4 | 1 cm | `gen: serpentine` |
| Viviparous lizard | Small brown lizard with a pale dorsal band, basking flat on a log | 0.06 (0.15 total) | 1 cm | `gen: quadruped` ⚠ |
| Stag beetle | Male has antler-like mandibles a third of body length; the largest beetle here and unmistakable | 0.08 | 1 cm | `gen: arthropod` ⚠ |

⚠ **Six lattice notes, and they divide into three different problems.**

* **Antlers (red deer, elk).** A main beam is 3–4 cm and a tine tip 1–2 cm on a
  2 m animal. At 5 cm they vanish and the stag becomes a hind; at 2 cm a beam is
  two voxels and a tine one — still under the three-voxel rule. **There is no
  lattice at which life-size round antlers read.** Draw them thicker than life at
  2 cm and record it in `notes`, exactly as the small birds were. Contrast the
  fallow deer and the moose, whose **palmate** antlers are broad flat blades and
  survive at 5 cm unaltered. That contrast decides which deer to build first.
* **Small dark marks on thin appendages** (squirrel tail plume, chipmunk stripes,
  wildcat tail rings, stoat tail tip, lynx ear tufts). All are 2–4 cm features. At
  1 cm each is two to four voxels and reads; at 2 cm most collapse to one voxel
  and the species becomes its generic relative. This is the fish research's
  2-on-2-off bar rule reappearing on mammals, and the same floor applies. **These
  species are 1 cm animals even though their bodies would have been happy at 5.**
* **Animals under ~20 cm** (dormouse at 8 cm, frog at 9 cm, viviparous lizard at
  15 cm total, stag beetle at 8 cm). At 1 cm these are 8–15 voxels, below the
  20-voxel floor every animal in this library respects. The dormouse is marked out
  of scope outright. The frog and the beetle are worth authoring above life size
  because each has a strong, simple, unmistakable silhouette that survives being
  drawn large; the lizard does not and probably should not be built.

## Fish

Cool, clear, fast water — the trout-and-grayling end of the freshwater range.
The engine tags a river by the land around it (`biomes.py:37-42`), so these are
the shaded, well-oxygenated streams a forest produces.

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Brown trout | Fusiform, spotted, with an adipose fin — the salmonid tell at this size | 0.30 | 1 cm | `shipped: brown-trout` |
| Northern pike | Long flat-headed sagittiform with the fins pushed right aft | 0.75 | 1 cm | `shipped: northern-pike` |
| European perch | Deep body, hard vertical bars, spiny first dorsal, red pelvics | 0.22 | 1 cm | `shipped: river-perch` |
| Mirror carp | Very deep body with a few large scattered scale plates and two barbel pairs | 0.90 | 2 cm | `shipped: mirror-carp` |
| Golden carp | Deep, bright orange-gold, no barbels | 0.40 | 1 cm | `shipped: golden-carp` |
| Pale minnow | Small silver fusiform, no marking | 0.20 | 1 cm | `shipped: pale-minnow` |
| European eel | Heavy anguilliform with a continuous fin ridge | 0.70 | 1 cm | `shipped: river-eel` |
| Atlantic salmon | Larger and more powerful than a trout, with a deeply forked tail, a narrow wrist, and X-shaped spots above the lateral line only | 0.9 | 2 cm | `queued` |
| Grayling | Trout-shaped under an enormous sail dorsal fin; the fin is the species and it is 8–10 cm tall, so it holds at 1 cm easily | 0.35 | 1 cm | `queued` |
| Brook trout | Dark olive with pale worm-track vermiculation on the back and white-edged lower fins — the white fin edges are the reliable mark | 0.28 | 1 cm | `queued` |
| Rainbow trout | Silver with a broad pink lateral band and dense fine black spots over the whole body and tail | 0.4 | 1 cm | `queued` |
| Bullhead / miller's thumb | Flat wide head, huge pectoral fins fanned on the bottom, no swim bladder so it sits on stones | 0.12 | 1 cm | `queued` ⚠ |
| Stone loach | Slim bottom-hugging body with six barbels and a mottled brown flank | 0.12 | 1 cm | `queued` ⚠ |
| Common minnow | Very small silver-olive shoaling fish with a dark broken lateral stripe | 0.09 | 1 cm | `queued` ⚠ |
| Chub | Blunt-headed heavy silver body with dark-edged scales | 0.45 | 1 cm | `queued` |
| Common dace | Slim small silver fish with a slightly concave dorsal edge | 0.2 | 1 cm | `queued` |
| Barbel | Long low flat-bellied body with four barbels and a high first dorsal | 0.7 | 1 cm | `queued` |
| Brook lamprey | An eel-shaped animal with **no jaws and no paired fins at all** — a round sucker mouth and seven gill pores in a row | 0.15 | 1 cm | `queued` ⚠ |
| Burbot | The only freshwater cod: long marbled body, one chin barbel, two dorsals | 0.5 | 1 cm | `queued` |
| Smallmouth bass | Bronze-olive with faint vertical bars and a jaw ending under the eye | 0.4 | 1 cm | `queued` |
| Bluegill | Very deep disc body, a solid black opercular flap, and faint vertical bars | 0.2 | 1 cm | `queued` |
| Common bream | Extremely laterally compressed bronze slab with a long anal fin | 0.5 | 1 cm | `queued` |
| Tench | Thick olive body, tiny red eye, rounded fins, one small barbel at each mouth corner | 0.45 | 1 cm | `queued` |

⚠ **Bullhead, stone loach, minnow, lamprey.** All 9–15 cm, so 9–15 voxels at
1 cm, under the 20-voxel floor. `clown-anemonefish` was authored at 22 cm against
a real 10 cm for exactly this reason and its `notes` say so. The lamprey is the
interesting one: it has no paired fins and no jaws, so there is nothing for the
fish generator's fin plates to do, and its whole identity is a row of seven gill
pores 1–2 cm apart. Author it at 25–30 cm or leave it out.

---

## Build priority

Temperate forest is under-built relative to how memorable it is: it has the best
tree set in the library and almost no floor, and a forest is mostly floor.

1. **Ferns and mosses.** Bracken, lady fern, sword fern, sphagnum, hair-cap moss.
   The tuft generator is shipped, and a temperate forest floor that is grass is
   simply the wrong biome. This is the largest correctness gap in the file, not
   just the largest volume gap. **Settle the feather-moss question first** — if a
   continuous mat should be a material rather than an asset, that decision changes
   what gets authored here.
2. **The spring flora, as carpets.** Bluebell, wood anemone, ramsons, primrose.
   Four tuft specs, and the bluebell in particular is the single strongest visual
   event available in this biome for one spec's work — provided placement can put
   it down as a dense sheet rather than scattered singles.
3. **Beech, hornbeam and yew.** Three trees whose identity is *bark and bole*
   rather than crown, which is the axis the shipped set — mostly oaks and
   conifers — does not cover. Beech also comes with a placement rule worth having:
   almost nothing grows under it.
4. **Trunk-clinging birds.** Nuthatch, treecreeper, green woodpecker, black
   woodpecker. The bird generator already poses `great-spotted-woodpecker`
   vertically, so the pose exists; these are parameter and palette work on a
   proven path, and they put birds on tree trunks rather than only in the air.
5. **The long-tailed tit**, out of order and on its own, because its identity is
   a pure proportion and proportions survive every lattice. Cheapest legible
   species in the file.
6. **Salmonids.** Salmon, grayling, brook trout, rainbow trout. Four species that
   are parameter variations on a shipped spec, and grayling's sail dorsal is a
   genuinely new silhouette for one slider's work.
7. **Then quadrupeds, when the generator lands**, in this order: **fallow deer
   first** (palmate antlers hold at 5 cm, spots are a shipped-style marking), then
   wild boar (no fine appendages to lose), red fox, badger, grey squirrel, brown
   bear. Leave red deer and elk until the antler thickness question has an answer,
   and build the lynx early **as a deliberate probe** of whether 2 cm holds ear
   tufts — because if it does not, five other species in this table are affected.

## Where the numbers come from

Every size in this file is an **approximate typical adult figure from general
knowledge**. None of it is measured, and none of it is sourced. It is good
enough to choose a voxel lattice, which is all this document is for, and it is
**not** good enough to quote as a fact or to paste into a spec's `notes` without
checking first.

The repo references — `biome.h` and `biomes.py` line numbers, `kinds.py:130` on
the bird generator's 20–90 voxel range, shipped spec names and their authored
sizes — were read out of the files and are exact. The **4.90%** land share is
taken from the 289-tile column of
`docs/measurements/biome-screenshot-targets-2026-08-01.txt`; the earlier 2.43%
figure in the same file is explicitly retracted by that file's CORRECTION 2 as a
small-sample artefact, and so is the confident claim that savanna's gate order is
what causes the shortfall. A gate-order effect is real; its magnitude is not
established.

Specific hedges:

* **The Lattice column is a recommendation derived from the three-voxel rule, not
  a measurement.** Only the shipped rows have a tested lattice. Every other row
  should be confirmed with `tools/fishprobe.py --lattice` or `tools/birdprobe.py
  --lattice` before a spec is written.
* **The antler and ear-tuft arithmetic is arithmetic on approximate figures**,
  not a measurement. The conclusion — that round antler tines fail the three-voxel
  rule at both 5 cm and 2 cm while palmate antlers pass at 5 cm — follows from
  those approximations and should be checked by rendering one before it is used
  to reorder anyone's work.
* **The root-split block and the feather-moss carpet are questions, not specs.**
  One asks for a composite of two generators, which nothing in the pipeline does;
  the other asks whether a continuous 2–5 cm mat is an asset at all. Both are in
  the file so the questions get answered rather than rediscovered.
* **Several species here overlap grassland and taiga**, and deliberately so —
  roe deer, badger, wildcat, fox, hare, bramble, hazel all span the boundary. The
  biome weights on the spec, not this file's headings, are the final word.
