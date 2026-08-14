# Grassland — master species list

**Biome id 2** — `vxc::BiomeId::GRASSLAND`, `voxel-core/include/voxelcore/biome.h:26`;
mirrored `asset-forge/forge/biomes.py:61-62`.

| | |
|---|---|
| Climate envelope | Two ways in (`biome.h:231-232`): **arid and not hot** — under 400 mm/yr with a mean annual temperature below 24 °C; or **semi-arid without a marked dry season** — 400–800 mm/yr failing either the 18 °C warm test or the 70% precipitation-seasonality test. |
| Surface material | grass (`MAT_GRASS`, `biome.h:243`) |
| Share of land (shipped world) | **28.06% — the largest biome in the world.** One tile (`-1_1`) measures 96.3% grassland (`docs/measurements/biome-screenshot-targets-2026-08-01.txt`, 289-tile column; an earlier 121-tile pass in the same file read 27.06% / 94.3% and is superseded by that file's own corrections) |
| Water present | yes — rivers, streams, ponds, oxbows |
| Asset kinds hosted | everything: `tree`, `bush`, `rock`, `grass`, `reed`, `flower`, `fish`, `cetacean`, `bird` (`biomes.py:62`, `_FLIES`) |

**Grassland is this world's catch-all, and the species list has to be built
knowing that.** Read `biome.h:231` carefully: every arid column that is not *hot*
falls to grassland rather than desert, and `biome.h:122-133` records that this is
deliberate and correct — the gate was re-examined at worldgen v22 and left alone,
because lowering the desert temperature threshold to make more deserts would put
half the world's dry temperate land under sand. So grassland absorbs cold steppe,
temperate prairie, Mediterranean scrub, semi-arid shrubland and ordinary
pastureland, all under one id.

That makes it **the highest-priority biome in the entire project** — it is more
than a quarter of all land — and it means the list below must span a wide
climate, from Mongolian steppe to Iberian dehesa to North American prairie. The
weighting system is what carries that: a species here should usually also carry a
weight in savanna, temperate forest or desert, and most of the shipped ones do.

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

Grassland trees are scattered, not closed canopy, so each one is seen alone and
in silhouette. That raises the bar on crown shape and lowers it on foliage
detail.

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Pedunculate oak | Massive short bole, heavy low limbs, broad domed crown wider than it is tall | 14 | `shipped: temperate-oak` |
| Field elm | Tall vase-shaped crown on a straight trunk, branches rising then arching out | 11 | `shipped: field-elm` |
| Weeping willow | Short thick trunk, dense curtain of pendulous shoots reaching the ground | 12 | `shipped: weeping-willow` |
| Cherry blossom | Small spreading crown, horizontal banded bark, dense pale bloom | 6.5 | `shipped: cherry-blossom` |
| Columnar cypress | Narrow dark green flame, nearly parallel-sided, no visible trunk | 13 | `shipped: columnar-cypress` |
| Hawthorn | Low, dense, thorny, irregular round-topped crown often leaning with the wind | 3.8 | `shipped: hawthorn-scrub` |
| Umbrella acacia | Flat-topped horizontal crown on a bare trunk with a hard shadow line under it | 8 | `shipped: savanna-acacia` |
| Dead standing tree | Bare, barkless, forked, no crown at all | 6 | `shipped: desert-dead` |
| Sapling | A single slim stem with a small sparse crown | 4.5 | `shipped: temperate-sapling` |
| Olive | Short gnarled multi-stemmed trunk, hollow with age, small grey-green crown | 8 | `queued` |
| Holm oak | Dense dark rounded evergreen dome on a short trunk — the dehesa tree | 12 | `queued` |
| Cork oak | As holm oak but with a thick fissured bark that is stripped to a dark red-brown band on the lower trunk; the two-tone trunk is the species | 12 | `queued` |
| Stone pine | Bare straight trunk carrying a single flat parasol of foliage at the very top | 18 | `queued` |
| Scots pine | Orange-red upper trunk under a high, open, irregular crown; the bark colour is the identifying feature | 20 | `queued` |
| Quaking aspen | Slim white-green trunk with black scar bands, narrow crown, grows in dense clonal stands | 18 | `queued` |
| Eastern cottonwood | Broad open crown, deeply furrowed grey bark, always beside water in dry country | 25 | `queued` |
| White poplar | Column of pale grey-white bark with a bright two-tone crown — dark above, white-felted below | 20 | `queued` |
| Rowan | Small, upright, feathery pinnate leaves and heavy orange-red berry clusters | 8 | `queued` |
| Wild pear | Small, thorny, narrow upright crown, very dense white spring bloom | 8 | `queued` |
| Crab apple | Low, wide, twisted, with small hard fruit; a hedge-line tree | 6 | `queued` |
| Bur oak | Very heavy horizontal limbs, deeply corky bark, the classic lone prairie oak | 15 | `queued` |
| Honey locust | Open feathery crown on a trunk armed with clusters of long branched thorns | 18 | `queued` |
| Common ash | Straight trunk, high open crown, black buds, ascending branches curling up at the tips | 20 | `queued` |
| Field maple | Small dense rounded crown, corky ridged twigs, hedgerow habit | 10 | `queued` |
| Blackthorn | A thicket-forming small tree rather than a shrub in old hedges: black bark, dense thorns, white bloom on bare wood | 4 | `queued` |
| Tamarack / larch | Conical conifer that is bare in winter — the only one; its silhouette changes with the season | 20 | `queued` |

## Rock types

Grassland already carries the largest shipped rock set of any climate biome,
because a rock in open grass is a landmark and the library was built for that.

| Rock | Voxel-artist description | Block size (m) | Status |
|---|---|---|---|
| Granite boulder | Rounded joint block with three families of flat faces surviving under the rounding | 1.6 | `shipped: granite-boulder` |
| Glacial erratic | An out-of-place block resting on the surface, unweathered relative to its ground, part-buried | 1.2 | `shipped: glacial-erratic` |
| Jointed granite tor | A stack of rectangular joint blocks with open horizontal partings | 1.2 | `shipped: jointed-granite-tor` |
| Corestone tor | Rounded corestones emerging from a weathered mantle | 1.3 | `shipped: corestone-tor` |
| Summit tor | The same at hill-top scale | 3.2 | `shipped: summit-tor` |
| Tor stack (hero) | Landmark-scale stacked block tower | 4.0 | `shipped: hero-tor-stack` |
| Standing stone | A single tall slab set upright, taller than wide, weathered on all faces | 2.2 | `shipped: standing-stone` |
| Fractured outcrop | Angular blocks in place with open joints between them | 1.5 | `shipped: fractured-outcrop` |
| Exfoliating dome | Smooth swelling dome shedding curved shells | 1.2 | `shipped: exfoliating-dome` |
| Limestone slab | Flat-lying bedded plate with a squared edge | 1.4 | `shipped: limestone-slab` |
| Limestone pinnacles | Sharp fluted spires in a cluster | 0.9 | `shipped: limestone-pinnacles` |
| Karren pavement | Flat bedrock floor cut into blocks by deep solution grooves | 1.1 | `shipped: karren-pavement` |
| Banded sandstone ledge | Horizontal colour-banded step with an undercut | 1.2 | `shipped: banded-sandstone-ledge` |
| Desert mesa block | Flat-topped resistant cap over a softer slope | 1.2 | `shipped: desert-mesa-block` |
| Basalt colonnade | Rank of hexagonal columns | 1.2 | `shipped: basalt-colonnade` |
| Basalt colonnade (hero) | The same at cliff scale | 1.2 | `shipped: hero-basalt-colonnade` |
| Fault breccia | Angular fragments of one rock cemented in a fine matrix | 1.2 | `shipped: fault-breccia` |
| Veined granite | Pale rock cut by darker or quartz-white veins at an angle to the faces | 1.2 | `shipped: veined-granite` |
| Mossy forest boulder | A rounded block with a moss cap on its upper surfaces only | 1.2 | `shipped: mossy-forest-boulder` |
| River cobble | Rounded flattened water-worn stone | 1.2 | `shipped: river-cobble` |
| Cliff-fall block | A large fresh angular block with sharp arrises, sitting where it landed | 2.8 | `shipped: cliff-fall-block` |
| Tsingy pinnacles (hero) | Forest of razor-edged limestone blades | 3.0 | `shipped: hero-tsingy-pinnacles` |
| Sarsen stone | A rounded silcrete block lying loose on chalk downland, very hard, pale grey, no bedding | 2.0 | `queued` |
| Chalk outcrop | Brilliant white blocky low outcrop with horizontal flint bands, crumbling at the edges | 1.5 | `queued` |
| Conglomerate boulder | A block visibly made of rounded pebbles set in a finer matrix — a texture and clast-size job on the existing generator | 1.4 | `queued` |
| Gravel bar | A low lens of well-sorted rounded stone on the inside of a river bend | 0.15 clast | `queued` |
| Erratic train | A line of blocks of the same lithology strung across the ground — a placement pattern more than a new asset | 1.2 | `queued` |
| Termite-mound-hard outcrop | Not a rock. Listed only to say it is **not** one: a termite mound is a biogenic structure and would be a separate asset if wanted | 2.0 | out of scope |

## Flowers

The most under-served category in the biome relative to its size: two shipped
specs for 28% of the world's land.

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Meadow daisy | Rosette of leaf stems with a few carrying a white-and-yellow disc bloom | 0.42 | `shipped: meadow-daisy` |
| Thrift / sea pink | Grassy cushion with bare stalks and tight pink pompoms | 0.26 | `shipped: coastal-thrift` |
| Common poppy | Wiry hairy stem, one nodding bud, then a wide open scarlet cup with a black centre | 0.6 | `queued` |
| Cornflower | Slim grey-green stem, one intense blue ragged-edged head | 0.7 | `queued` |
| Oxeye daisy | Taller and larger than the meadow daisy, one head per stem, dark green toothed leaves | 0.7 | `queued` |
| Yarrow | Flat-topped dense white plate of tiny florets over very finely divided feathery leaves | 0.6 | `queued` |
| Common knapweed | Stiff branched stem, hard scaly bud, purple thistle-like brush without spines | 0.8 | `queued` |
| Field scabious | Long bare stem with one flat lilac pincushion head | 0.8 | `queued` |
| Harebell | Very slender wiry stem with a few nodding pale blue bells; the thinnest silhouette in the list | 0.35 | `queued` ⚠ |
| Cowslip | Tight rosette with one stalk carrying a nodding one-sided cluster of deep yellow tubes | 0.25 | `queued` |
| Wild thyme | Low creeping woody mat with dense pink-purple flower heads; a mat, not a stem | 0.1 | `queued` |
| Red clover | Trefoil leaves each with a pale chevron, and a dense round pink-purple head | 0.4 | `queued` |
| Bird's-foot trefoil | Sprawling low stems with clusters of yellow-and-orange pea flowers | 0.25 | `queued` |
| Common chicory | Tall stiff nearly leafless branched stem with bright sky-blue daisies pressed against it | 1.0 | `queued` |
| Viper's bugloss | Bristly upright spike covered in funnel flowers that open pink and turn blue, with long protruding stamens | 0.7 | `queued` |
| Purple coneflower | Stiff stem, drooping mauve petals around a tall bristly orange-brown cone; the cone is the species | 0.9 | `queued` |
| Prairie coneflower | Very tall thin stem with a long dark cylindrical cone and few reflexed yellow petals | 1.0 | `queued` |
| Blazing star / liatris | A dense vertical bottlebrush spike of purple that opens from the **top down** | 1.2 | `queued` |
| Common milkweed | Thick upright stem, big paired leaves, domed umbels of dusty pink, later spindle-shaped pods | 1.4 | `queued` |
| Lupine | A bold vertical spike of stacked pea flowers over a palmate whorl of leaflets | 0.9 | `queued` |
| California poppy | Low blue-grey feathery mound with silky four-petal orange cups | 0.35 | `queued` |
| Common sunflower (wild) | Branched, rough, hairy, with many small heads rather than one large one | 2.0 | `queued` |

⚠ **Harebell.** Stem 1–2 mm, flower 15 mm, on a 5 cm ground-cover lattice. The
flower is a third of a voxel. Either author the bloom oversize as a single voxel
of a distinct colour on a one-voxel stem — which is what the tuft generator
already does for `meadow-daisy` and is legible — or leave it out. It is listed
because "a wildflower meadow with no harebells" is a decision, and decisions
should be recorded.

## Ground cover

Includes reeds, which are their own kind (`biomes.py:35`) sharing the tuft
generator.

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Meadow grass | Baseline soft tuft of fine blades | 0.45 | `shipped: meadow-grass` |
| Dry tussock | Coarse pale bunched tuft with dead straw in the centre | 0.62 | `shipped: dry-tussock` |
| Bulrush | Stiff stems with dark brown cigar heads | 1.4 | `shipped: bulrush` |
| Common reed | Tall canes with a purple-brown feather plume | 2.0 | `shipped: water-reed` |
| Pampas plume | Very tall stiff stem with a huge silver-white feather plume | 1.7 | `shipped: pampas-plume` |
| Big bluestem | Very tall prairie bunchgrass with a distinctive three-part "turkey foot" seed head and blue-grey stem bases | 2.0 | `queued` |
| Little bluestem | Shorter dense bunch turning copper-orange in autumn with fluffy white seed | 0.9 | `queued` |
| Switchgrass | Upright clump with a very open airy seed panicle that reads as a haze, not a head | 1.5 | `queued` |
| Buffalo grass | Low, fine, grey-green sod-forming turf — the shortgrass prairie floor | 0.15 | `queued` |
| Feather grass | Fine tussock with long silky awns that stream horizontally in wind; the awns are the species | 0.8 | `queued` |
| Sheep's fescue | Very fine dense blue-green hemispherical cushion | 0.2 | `queued` |
| Cocksfoot | Coarse tussock with a lopsided one-sided seed head like a clenched hand | 1.0 | `queued` |
| Timothy | Single stiff stems each topped with a long dense cylindrical head | 0.9 | `queued` |
| Ribwort plantain | Flat rosette of ribbed lance leaves with bare stalks carrying a short brown head ringed by white stamens | 0.3 | `queued` |
| Sedge tussock | A rising mound of its own dead leaves with living blades on top — an old tussock is half dead matter | 0.6 | `queued` |
| Moss cushion | Low dense green dome in a hollow or a stone's lee | 0.06 | `queued` |
| Reed sweet-grass | Broad soft bright green blades in wet hollows, arching over | 1.5 | `queued` |
| Bracken (dry edge) | Coarse triangular fronds on a bare stalk, in a dense uniform stand | 1.2 | `queued` |

## Bushes / shrubs

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Bramble thicket | Dense arching thorny canes that root where they touch, forming an impassable mound | 1.5 | `shipped: bramble-thicket` |
| Juniper scrub | Low dark spreading prostrate conifer with a hard grey-green texture | 1.1 | `shipped: juniper-scrub` |
| Coastal scrub | Wind-shorn flat-topped multi-stem shrub | 0.9 | `shipped: coastal-scrub` |
| Desert shrub | Sparse grey woody twiggy mound with little foliage | 1.0 | `shipped: desert-shrub` |
| Gorse | Dense spiny evergreen mound in solid yellow bloom; the spines *are* the leaves | 1.8 | `queued` |
| Common broom | Upright bundle of green whippy ridged stems, nearly leafless, with yellow pea flowers | 2.0 | `queued` |
| Big sagebrush | Silver-grey aromatic mound on a short twisted woody trunk — the defining shrub of cold semi-desert steppe | 1.5 | `queued` |
| Rubber rabbitbrush | Rounded pale blue-green shrub topped with a flat mass of bright yellow flower | 1.2 | `queued` |
| Hazel | Multi-stemmed from ground level, tall straight poles, broad round leaves | 4 | `queued` |
| Dog rose | Long arching thorny canes, sparse pink flowers, scarlet flask-shaped hips | 2.5 | `queued` |
| Blackthorn scrub | Dense black thorny suckering thicket, white flowers on bare wood, blue-black sloes | 3 | `queued` |
| Sea buckthorn | Grey thorny thicket with narrow silver leaves and heavy orange berries on the stems | 2.5 | `queued` |
| Snowberry | Loose arching twiggy shrub with conspicuous white marble berries in clusters | 1.5 | `queued` |
| Spindle | Slender green four-angled twigs, and bright pink four-lobed fruit splitting to orange seed | 3 | `queued` |
| Guelder rose | Open shrub with maple-like leaves, flat white flower heads and translucent red berries | 3 | `queued` |
| Box | Very dense small dark evergreen leaves on a tight woody frame; reads as a solid mass | 2 | `queued` |
| Wild privet | Semi-evergreen dense twiggy hedge shrub with black berry clusters | 2.5 | `queued` |
| Saltbush | Low grey-white mealy mound with no thorns, on alkaline ground | 1.0 | `queued` |

## Birds

Grassland has the largest shipped bird set of any biome (16 of 20), which makes
it the best place to see what the generator can already do — and the queued list
below is aimed at what it cannot: the ground-nesting, long-legged and
display-postured species.

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Common buzzard | Broad-winged raptor, short fanned tail, mottled brown with a pale chest crescent | 0.52 | 1 cm | `shipped: common-buzzard` |
| Golden eagle | Very large, long-fingered wings, golden nape, heavy hooked bill | 0.85 | 1 cm | `shipped: golden-eagle` |
| Common kestrel | Small falcon, pointed wings, long barred tail, hovering posture | 0.34 | 1 cm | `shipped: common-kestrel` |
| Common raven | Big, wedge tail, heavy bill; identified by all three in silhouette | 0.64 | 1 cm | `shipped: common-raven` |
| Eurasian jay | Pinkish body, black moustache, white rump, one bright blue barred wing patch | 0.34 | 1 cm | `shipped: eurasian-jay` |
| Common starling | Compact, short-tailed, triangular-winged, spangled | 0.21 | 1 cm | `shipped: common-starling` |
| Song thrush | Warm brown above, cream below with dense arrow-shaped spots | 0.23 | 1 cm | `shipped: song-thrush` |
| European robin | Round, upright, orange-red face and breast. **Authored at 24 cm against a real 14 cm** | 0.24 | 1 cm | `shipped: european-robin` |
| Great tit | Black head with white cheeks, yellow below with a black belly stripe. **Authored at 24 cm against a real 14 cm** | 0.24 | 1 cm | `shipped: great-tit` |
| Barn swallow | Forked tail streamers, blue-black above, red throat. **Authored at 26 cm against a real 17–19 cm** | 0.26 | 1 cm | `shipped: barn-swallow` |
| Eurasian hoopoe | Pink-buff body, black-and-white barred wings, long decurved bill and a huge fannable crest | 0.27 | 1 cm | `shipped: eurasian-hoopoe` |
| Common kingfisher | Tiny, dagger-billed, electric blue above and orange below. **Authored at 20 cm against a real 17 cm** | 0.20 | 1 cm | `shipped: common-kingfisher` |
| Rock pigeon | Stocky grey, two black wing bars | 0.33 | 1 cm | `shipped: rock-pigeon` |
| Mallard | Green head, white collar, heavy body | 0.58 | 1 cm | `shipped: mallard-duck` |
| Grey heron | Long S-neck, dagger bill, black crest plume | 1.00 | 1 cm | `shipped: grey-heron` |
| Herring gull | White and grey with black wingtips | 0.60 | 1 cm | `shipped: herring-gull` |
| Eurasian skylark | Small streaky brown bird with a short blunt crest, drawn hanging in a vertical hover; the *pose* is the species | 0.18 | 1 cm | `queued` ⚠ authored-up |
| Corn bunting | Fat, dull, streaky brown with no marking at all, sitting on a wire or stem with legs dangling in flight | 0.18 | 1 cm | `queued` ⚠ authored-up |
| Meadow pipit | Slim streaky brown with white outer tail feathers that only show in flight | 0.15 | 1 cm | `queued` ⚠ authored-up |
| Northern lapwing | Broad rounded wings, glossy dark green back, white belly, and a long thin upswept head crest | 0.30 | 1 cm | `queued` |
| Grey partridge | Round, short-tailed, ground-hugging, grey with an orange face and a dark horseshoe on the belly | 0.30 | 1 cm | `queued` |
| Common quail | Tiny round game bird, almost never seen off the ground, streaked buff | 0.18 | 1 cm | `queued` ⚠ authored-up |
| Common pheasant | Very long pointed tail half the total length, red face wattle, iridescent green head, white neck ring | 0.80 | 1 cm | `queued` |
| Great bustard | The heaviest flying bird here: barrel body, thick neck, long legs, and a male display that turns it inside out into a white ball | 1.05 | 1 cm | `queued` |
| Common crane | Very tall, long neck and legs, grey with a black-and-white head stripe and a drooping bustle of tertials over the tail | 1.15 | 1 cm | `queued` |
| White stork | White with black flight feathers, long red bill and red legs, standing on one leg | 1.10 | 1 cm | `queued` |
| Hen harrier | Slim long-winged raptor quartering low with wings in a shallow V; male pale grey, female brown with a white rump | 0.48 | 1 cm | `queued` |
| Red kite | Long-winged with a deeply forked rusty tail that twists constantly — the fork is the species | 0.65 | 1 cm | `queued` |
| Little owl | Very small flat-headed owl with a fierce white brow, perched upright on a post | 0.23 | 1 cm | `queued` |
| Northern wheatear | Grey back, black mask and wings, and a bold white rump with a black inverted T on the tail | 0.16 | 1 cm | `queued` ⚠ authored-up |
| European stonechat | Round upright small bird, black head, white half-collar, orange breast | 0.13 | 1 cm | `queued` ⚠ authored-up |
| Yellowhammer | Bright lemon head and underparts with a chestnut rump, long-tailed for its size | 0.17 | 1 cm | `queued` ⚠ authored-up |
| European goldfinch | Red face, black-and-white head, broad gold wing bar on black | 0.13 | 1 cm | `queued` ⚠ authored-up |
| Eurasian magpie | Black and white with an iridescent tail longer than the body | 0.46 | 1 cm | `queued` |
| Carrion crow | All black, square tail, straight heavy bill — the shape `common-raven` is defined against | 0.48 | 1 cm | `queued` |
| Western jackdaw | Small crow with a pale grey nape shawl and a pale eye | 0.34 | 1 cm | `queued` |
| Burrowing owl | Long-legged small owl standing at a burrow mouth, spotted brown, no ear tufts | 0.23 | 1 cm | `queued` |
| Greater rhea | Large flightless ratite, grey-brown, long neck and legs, wings used as a shawl | 1.40 | 2 cm | `queued` |
| Western meadowlark | Brown streaked above, brilliant yellow below with a black V on the chest | 0.23 | 1 cm | `queued` |

⚠ **The small-passerine floor, restated.** Eight species above are 13–18 cm. At
1 cm that is 13–18 voxels, and the bird research recorded that a perched songbird
is authored at 36–42° nose-up so 20 cm projects onto only sixteen voxels of
length — which is why four birds in the library are already authored at 20–26 cm
against real lengths of 14–19 cm, each with the reason in its own `notes`. Every
row flagged here needs the same treatment. **Write the reason down in the spec**,
or the next person will "correct" it back to life size and quietly destroy the
bird.

## Land animals

**This is the biggest single gap in the project.** There is no quadruped
generator, so every row here is blocked, and grassland is 28% of the world's
land. Nothing else in this document is worth as much as unblocking this table.

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| American bison | Low slab body, forequarter hump twice the height of the hips, shaggy dark cape over the shoulders and a bare rear, short pale horns curving out then up | 2.8 / 1.8 sh | 5 cm | `gen: quadruped` |
| European bison / wisent | Taller, longer-legged, less humped and less shaggy than the American; the difference is proportion only | 2.9 / 1.9 sh | 5 cm | `gen: quadruped` |
| Red deer stag | Long-legged deer with a thick maned neck and branched antlers spreading nearly a metre with 5–7 tines a side. **The antlers are the whole silhouette** | 2.0 / 1.2 sh | 2 cm | `gen: quadruped` ⚠ |
| Red deer hind | The same animal with no antlers and a slimmer neck — a genuinely different silhouette from one field | 1.9 / 1.1 sh | 5 cm | `gen: quadruped` |
| Roe deer | Small, short-bodied, tall-rumped, with a bright white rump patch and short three-point antlers | 1.2 / 0.7 sh | 2 cm | `gen: quadruped` |
| Fallow deer | Mid-sized, boldly white-spotted on chestnut, with palmate (flattened, blade-like) antlers rather than round tines — palmate antlers survive a coarse lattice where round tines do not | 1.5 / 0.95 sh | 5 cm | `gen: quadruped` |
| Pronghorn | Very slender fast antelope-shaped animal, tan with two bold white throat bands and a white rump; short forked horns | 1.4 / 0.9 sh | 2 cm | `gen: quadruped` |
| Saiga antelope | Ordinary antelope body under an absurd bulbous downturned nose; the nose is the only thing anyone will look at | 1.3 / 0.7 sh | 2 cm | `gen: quadruped` |
| Przewalski's horse | Stocky dun horse with a stiff upright black mane and no forelock, a dark dorsal stripe and faint leg barring | 2.1 / 1.3 sh | 5 cm | `gen: quadruped` |
| Guanaco | Long-necked camelid, tan above with a hard white line to the belly and a grey head | 1.9 / 1.1 sh | 5 cm | `gen: quadruped` |
| Grey wolf | Long-legged dog, deep narrow chest, straight bushy tail carried low, heavy ruff | 1.3 / 0.8 sh | 2 cm | `gen: quadruped` |
| Coyote | Smaller and finer than a wolf, larger ears in proportion, tail carried down when running | 0.9 / 0.55 sh | 2 cm | `gen: quadruped` |
| Red fox | Slender, black-backed pointed ears, white-tipped brush as thick as the body | 0.7 / 0.4 sh | 2 cm | `gen: quadruped` |
| Corsac fox | Paler, shorter-eared, shorter-legged steppe fox | 0.55 / 0.3 sh | 2 cm | `gen: quadruped` |
| Wild boar | Wedge body heaviest at the shoulder, head carried low, short legs, coarse bristle, small tusks on the male | 1.4 / 0.8 sh | 5 cm | `gen: quadruped` |
| European hare | Very long black-tipped ears, long hind legs, running low with the ears laid back; the ears are the species | 0.65 / 0.25 sh | 1 cm | `gen: quadruped` ⚠ |
| Black-tailed jackrabbit | Even longer ears in proportion, black-topped tail, sandy grey | 0.55 / 0.25 sh | 1 cm | `gen: quadruped` ⚠ |
| European rabbit | Shorter ears, rounder body, white tail flash when fleeing | 0.4 / 0.2 sh | 1 cm | `gen: quadruped` |
| European badger | Very low, wide, wedge-shaped, grizzled grey with a black-and-white striped face; the face stripes are the species | 0.8 / 0.3 sh | 2 cm | `gen: quadruped` |
| Alpine marmot | Fat short-legged ground squirrel sitting bolt upright on its haunches; the upright pose is how it is recognised | 0.5 / 0.2 sh | 1 cm | `gen: quadruped` |
| European souslik | Smaller, slimmer, standing vertical at a burrow mouth, sandy with faint pale flecks | 0.22 | 1 cm | `gen: quadruped` ⚠ |
| Black-tailed prairie dog | Stout tan ground squirrel with a short black-tipped tail, standing at a mound | 0.35 | 1 cm | `gen: quadruped` |
| Eastern grey kangaroo | Sits back on a heavy tail as a third leg, tiny forelimbs, huge hindquarters, upright neck and long ears | 1.3 / 1.5 tall | 5 cm | `gen: quadruped` |
| Red kangaroo | Larger, redder, longer-faced, and the most upright of the group | 1.6 / 1.8 tall | 5 cm | `gen: quadruped` |
| Emu | Not a quadruped — a very large flightless bird, shaggy grey-brown with a bare blue neck. Could plausibly be the **bird** generator run at 2 cm with no flight pose | 1.6 tall | 2 cm | `queued` (bird gen) |
| European wildcat | Heavier than a domestic cat with a blunt, thick, **ringed and black-tipped** tail — the tail is the only reliable separator | 0.6 / 0.35 sh | 1 cm | `gen: quadruped` ⚠ |
| Steppe polecat | Long low mustelid, pale straw body with dark legs and a dark face mask | 0.5 | 1 cm | `gen: quadruped` |
| Stoat | Very small, very long, chestnut above and cream below with a **black tail tip** that stays black in winter when the rest turns white | 0.3 | 1 cm | `gen: quadruped` ⚠ |
| European hedgehog | A spine-covered dome with a small pointed face and short legs; the spines are a surface texture, not geometry | 0.25 | 1 cm | `gen: quadruped` |
| European mole | A cylinder with no visible neck, no visible eyes, and two enormous outward-facing shovel forepaws | 0.15 | 1 cm | `gen: quadruped` ⚠ |
| Nine-banded armadillo | A jointed armour shell in bands over a low body, long tapering armoured tail, large ears | 0.5 (0.9 total) | 2 cm | `gen: quadruped` |
| Maned wolf | Absurdly long black legs under a small fox-red body, large ears, black dorsal mane | 1.0 / 0.9 sh | 2 cm | `gen: quadruped` |
| Steppe tortoise | Low domed shell with a squared-off front, stumpy elephantine legs | 0.2 shell | 1 cm | `gen: chelonian` |
| Ocellated lizard | Large green lizard with rows of blue eye-spots along the flank, heavy head, long tail | 0.25 (0.7 total) | 1 cm | `gen: quadruped` |
| Sand lizard | Small, stocky, brown with a pale dorsal band; the male turns bright green on the flanks in spring | 0.09 (0.2 total) | 1 cm | `gen: quadruped` ⚠ |
| Grass snake | Olive-green with a yellow-and-black collar behind the head; a smooth taper from head to tail tip | 1.0 | 1 cm | `gen: serpentine` |
| European adder | Shorter, thicker, with a hard black zigzag down the spine and a V on the head | 0.6 | 1 cm | `gen: serpentine` ⚠ |
| Field vole / harvest mouse | 6–10 cm. **Below every lattice this project has as a distinguishable animal.** Listed to say so | 0.08 | — | out of scope |

⚠ **Six lattice notes, and they are different problems.**

* **Red deer stag — antlers.** A main beam is 3–4 cm thick and a tine tip is
  1–2 cm, on a 2 m animal. At 5 cm the antlers vanish completely and the stag
  becomes a hind. At 2 cm a beam is two voxels and a tine tip is one — still
  under the three-voxel rule. **There is no lattice at which life-size red deer
  antlers read.** The honest fix is the one the library already used for small
  birds: draw the antlers thicker than life, at 2 cm, and put the reason in
  `notes`. Compare the fallow deer, whose *palmate* antlers are 10–15 cm broad
  blades and survive at 5 cm unmodified — that contrast is the most useful thing
  in this table, because it says which deer to build first.
* **Hare and jackrabbit — ears.** Ear length 10–12 cm, ear *width* 3–4 cm, on a
  65 cm animal. At 2 cm the ear is two voxels wide, at 1 cm it is three or four
  and reads properly. The ears set the lattice; the body would have been happy at
  5 cm.
* **Wildcat tail rings, stoat tail tip.** Both are a small dark mark on a thin
  appendage: the wildcat's rings are ~3 cm bands on a 3 cm-thick tail, the
  stoat's tip is ~3 cm on a 12 cm tail. At 1 cm each is three voxels and works.
  At 2 cm the rings collapse to one voxel each and the wildcat becomes a
  house cat. This is the fish research's 2-on-2-off bar rule appearing on a
  mammal, and the same floor applies.
* **Souslik, sand lizard, mole.** All 9–22 cm. A 22 cm animal at 1 cm is 22
  voxels, which is right at the shipped floor for a bird — and a mammal has no
  bill or tail fan to spend length on, so it is a lozenge with legs. Author these
  above life size or leave them out.
* **Adder zigzag.** The zigzag is the only thing separating an adder from every
  other brown snake, and its bands are 2–3 cm on a 6 cm-wide body. At 1 cm it
  works and nothing coarser does.
* **Emu.** Included in this table because a player calls it an animal, but it
  should be built with the **bird** generator, not a quadruped one. It is worth
  doing early for exactly that reason: it is a large, visible, distinctive
  grassland animal that needs no new generator at all.

## Fish

Grassland rivers and ponds. The engine tags a river by the land around it
(`biomes.py:37-42`), so these are "fish of a river running through open
country" — slower, warmer, siltier water than the taiga or temperate-forest
lists.

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Brown trout | Fusiform, spotted rather than striped, with an adipose fin — the salmonid tell | 0.30 | 1 cm | `shipped: brown-trout` |
| Northern pike | Sagittiform: long, flat-headed, with dorsal and anal fins pushed right to the tail | 0.75 | 1 cm | `shipped: northern-pike` |
| European perch | Deep body with hard vertical bars and a spiny first dorsal, red pelvic fins | 0.22 | 1 cm | `shipped: river-perch` |
| Mirror carp | Very deep heavy body with a few large scattered scale plates and two pairs of barbels | 0.90 | 2 cm | `shipped: mirror-carp` |
| Golden carp | Smaller, deeper, bright orange-gold with no barbels | 0.40 | 1 cm | `shipped: golden-carp` |
| Mud catfish | Flat wide head, long barbels, low scaleless body, dark mottled | 0.38 | 1 cm | `shipped: mud-catfish` |
| Pale minnow | Small silver fusiform, no marking | 0.20 | 1 cm | `shipped: pale-minnow` |
| European eel | Heavy anguilliform, continuous fin ridge | 0.70 | 1 cm | `shipped: river-eel` |
| Common bream | Very deep, extremely laterally compressed, bronze, with a long anal fin — the flattest fish here | 0.5 | 1 cm | `queued` |
| Tench | Thick-set olive-green body, tiny red eye, very rounded fins, one small barbel at each mouth corner | 0.45 | 1 cm | `queued` |
| Roach | Silver, red eye, orange-red fins; separated from a rudd only by mouth and fin position | 0.25 | 1 cm | `queued` |
| Rudd | As roach but with an upturned mouth and the dorsal set further back | 0.28 | 1 cm | `queued` |
| Chub | Blunt-headed heavy silver body with dark-edged scales and a large white mouth | 0.45 | 1 cm | `queued` |
| Common dace | Slim, small, silver, with a slightly concave dorsal edge | 0.2 | 1 cm | `queued` |
| Barbel | Long low bottom-hugging body, flat-bellied, with four barbels and a very high first dorsal | 0.7 | 1 cm | `queued` |
| Gudgeon | Small, spotted, bottom-living with a downturned mouth and two barbels | 0.13 | 1 cm | `queued` ⚠ |
| Zander / pikeperch | Pike-like head on a perch-like body with a spiny first dorsal and glassy eyes | 0.7 | 1 cm | `queued` |
| Wels catfish | Enormous flat-headed scaleless catfish with six barbels, a tiny dorsal and an anal fin running half the body | 2.0 | 5 cm | `queued` |
| Crucian carp | Deep, humped, golden-bronze with rounded fins and no barbels at all | 0.3 | 1 cm | `queued` |
| Burbot | The only freshwater cod: long body, one chin barbel, two dorsals, marbled brown | 0.5 | 1 cm | `queued` |
| Three-spined stickleback | Tiny, with three erect spines before the dorsal and a red throat on the breeding male | 0.06 | 1 cm | `queued` ⚠ |
| Grayling | Trout-shaped with an enormous sail-like dorsal fin — the fin is the species | 0.35 | 1 cm | `queued` |

⚠ **Gudgeon and stickleback.** 6–13 cm at 1 cm is 6–13 voxels, well under the
20-voxel floor the library set. `clown-anemonefish` was authored at 22 cm against
a real 10 cm for exactly this reason and its `notes` say so. Do the same or leave
them out; a 6-voxel stickleback is a chip of colour, not a fish.

---

## Build priority

**Grassland is the first biome to fill, before every other, because it is 28.06%
of the world's land** — more than a quarter of everywhere a player can stand. An
hour spent here is worth more than an hour spent anywhere else in this document.

1. **Wildflowers.** Two flower specs for a quarter of the world is the single
   most visible deficit in the library. Poppy, cornflower, oxeye daisy, yarrow,
   knapweed, clover, thyme — seven tuft specs on a shipped, proven generator,
   each of which changes the colour of a whole hillside. Nothing else costs this
   little per unit of visible change.
2. **Prairie and steppe grasses.** Big bluestem, little bluestem, feather grass,
   buffalo grass. Same generator, and they fix the other half of the same
   problem: a grassland biome currently has two grass specs and a tussock.
3. **The open-country birds that are not songbirds** — lapwing, pheasant, crane,
   white stork, red kite, grey partridge. Large, high-contrast, and each one is a
   distinct silhouette rather than another 20-voxel brown lozenge. The bird
   generator handles all of them, and the crane and the stork exercise the
   long-neck/long-leg end of it that only `grey-heron` currently reaches.
4. **The emu.** One large, unmistakable, animal-shaped entity that needs **no new
   generator**. It is the cheapest possible answer to "the world has no animals".
5. **A dozen more scattered trees.** Holm oak, stone pine, aspen, bur oak, olive.
   A lone tree in open grass is the most-looked-at object in the biome.
6. **Then the quadrupeds, the moment the generator exists** — in this order:
   bison (largest, most iconic, simple horns), fallow deer (palmate antlers
   survive a coarse lattice), wild boar (no appendages to lose), red fox, hare.
   Leave the red deer stag until the antler question above has an answer.

## Where the numbers come from

Every size in this file is an **approximate typical adult figure from general
knowledge**. None of it is measured, and none of it is sourced. It is good
enough to choose a voxel lattice, which is all this document is for, and it is
**not** good enough to quote as a fact or to paste into a spec's `notes` without
checking first.

The repo references — `biome.h` and `biomes.py` line numbers, the 28.06% land
share and the 96.3% best tile from the **289-tile column** of
`docs/measurements/biome-screenshot-targets-2026-08-01.txt` (that file's earlier
121-tile pass read 27.06% / 94.3% and it carries two explicit corrections saying
the smaller sample misled it), shipped spec names
and their authored sizes — were read out of the files and are exact.

Specific hedges:

* **The Lattice column is a recommendation derived from the three-voxel rule, not
  a measurement.** Only the shipped rows have a tested lattice. Every `queued` and
  `gen:` row's lattice should be confirmed with the relevant probe before a spec
  is written, exactly as `tools/fishprobe.py --lattice` and `tools/birdprobe.py
  --lattice` did for the shipped sets.
* **The antler arithmetic in the land-animal note is arithmetic, not a
  measurement.** The beam and tine thicknesses are approximate from general
  knowledge; the conclusion that they fall under the three-voxel rule at both 5 cm
  and 2 cm follows from those approximations. It should be checked by rendering
  one before it is used to justify redesigning anything.
* **Biome assignment is looser here than anywhere else in this document**, and
  deliberately so, because this biome is a catch-all spanning cold steppe to
  Mediterranean scrub. Several species below would in a finer classification sit
  in savanna (pronghorn, maned wolf), desert (corsac fox, jackrabbit) or
  temperate forest (badger, roe deer, wildcat). They are here because this world's
  grassland gate collects all of those climates. Use the biome weights, not this
  file's headings, as the final word.
