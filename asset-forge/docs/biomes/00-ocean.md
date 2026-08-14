# Ocean — master species list

**Biome id 0** — `vxc::BiomeId::OCEAN`, `voxel-core/include/voxelcore/biome.h:24`;
mirrored `asset-forge/forge/biomes.py:57-58`.

| | |
|---|---|
| Gate | Surface below −3 m (`kBiomeBeachLowerMm`, `biome.h:75`). A **morphology** gate — `biome.h:216` returns before climate is looked at, so nothing about temperature or rainfall reaches this biome. |
| Surface material | mud (`MAT_MUD`, `biome.h:241`) |
| Share of land | none — it is not land. It does not appear in the land census at all. |
| Water present | it *is* the water |
| Asset kinds hosted | `fish`, `cetacean`, `bird` — and nothing else (`biomes.py:58`) |

Ocean is the biome with the shortest hosted-kind list in the world and the
longest plausible species list, because six of the eight categories below are
not hosted at all. `plantable` is `False` (`biomes.py:58`) and `rock` is absent
from the tuple, so today the sea floor cannot carry a boulder, a reef or a
kelp frond. Everything in the water column can be built now; everything on the
bottom needs an engine-side change first.

## How to read the tables

**Status** is one of:

* `shipped: <spec-name>` — a spec already exists in `asset-forge/specs/`.
* `queued` — a generator exists for this kind; this is authoring work only.
* `gen: <name>` — needs a generator that does not exist. See `README.md` for the gap list.
* `host: <kind>` — the species is right for the place, but this biome's `hosts`
  tuple in `biomes.py` does not admit that kind yet. That is a one-line engine-side
  change, not authoring work, and it is called out because the list is otherwise
  silently undeliverable.

**Lattice** is the voxel size the asset should be authored at, by the house rule
in `forge/kinds.py:29-58`: a species is drawn at the coarsest voxel size at which
its smallest identifying feature is still about three voxels across.

**⚠** marks a species whose defining feature is at or below what the lattice will
hold, explained in the note under its table.

---

## Trees

**Not hosted.** `biomes.py:58` lists `("fish", "cetacean", "bird")` and
`plantable` is `False`. There is no tree in the sea.

The nearest thing that genuinely exists is a **giant kelp forest**, which reads
like a tree canopy from below and is 20–40 m tall — taller than every tree in
the library except `hero-sequoia`. It is listed under Ground cover rather than
here, because a kelp stipe is a strap and not a trunk, and because the generator
that comes closest is the reed tuft, not the tree.

## Rock types

**Not hosted.** `rock` is absent from ocean's `hosts` tuple, so a reef, a
seamount flank or a boulder field on the bottom cannot be placed today. This is
the single largest gap in the biome and it costs one word in `biomes.py:58`.

The rock generator itself needs nothing new for most of these — an accreted,
faceted, part-buried lump is exactly what a submerged boulder is. Only the
branching reef forms are a different shape.

| Rock | Voxel-artist description | Block size (m) | Status |
|---|---|---|---|
| Submerged granite boulder | The existing rounded joint block, part-buried in mud rather than soil; the difference is the palette and the burial fraction, not the geometry | 1.6 | `host: rock` — geometry is `shipped: granite-boulder` |
| Boulder reef | A loose cluster of angular blocks with wide gaps, sitting proud of the bottom | 1.0–2.5 | `host: rock` |
| Bedrock ledge / pinnacle | A flat-topped shelf stepping down in two or three risers, undercut at the base | 2–4 | `host: rock` |
| Seamount flank | A single large steep-sided cone section, no burial, sharp facets | 4–8 | `host: rock` |
| Sea cave mouth | A dark arched void cut into a ledge face; the identifying feature is the arch, not the rock | 3–5 | `host: rock` |
| Rubble apron | Fine angular scree fanning out from a ledge foot | 0.3–0.8 | `host: rock` |
| Branching stony coral head | A dense thicket of finger-thick branches on a hemispherical base; the branch, at 3–5 cm, is the smallest identifying feature and it decides the lattice | 0.5–2.0 | `gen: coral` |
| Massive brain coral | A boulder-shaped dome with a meandering surface groove pattern — geometrically a rock with a texture, which is why this one probably *is* the rock generator plus a palette | 0.5–3.0 | `gen: coral` (likely `rock` + palette) |
| Plate / table coral | A single flat horizontal disc on a short central stem, like a mushroom cap in stone | 1–3 | `gen: coral` |
| Coralline algal crust | A pink-purple encrusting film over rock. Millimetres thick — this is never geometry at any lattice this project has, and belongs in the material palette | — | material, not an asset |

> **The coral question, stated honestly.** Two of the three coral forms above are
> a rock with a different palette, and one — the branching thicket — is not: it
> is a self-similar branching structure, which is what `forge/skeleton.py`'s
> `grow` already builds for trees. So the shortest route to a reef is probably
> *the tree generator with a stone palette and no leaves*, not a new kind. That
> is a claim worth testing before anyone writes a coral generator, not a
> conclusion.

## Flowers

**Not hosted**, and almost not applicable — but not quite. **Seagrasses are true
flowering plants**, the only ones that live fully submerged in the sea, and a
seagrass meadow is a real, distinctive, buildable habitat. It is listed under
Ground cover below because the tuft generator is what would build it.

## Ground cover

**Not hosted.** `plantable` is `False`, and none of `grass`, `reed` or `flower`
appears in the tuple. Everything here needs the same `hosts` change the rocks do.

| Species | Voxel-artist description | Height (m) | Status |
|---|---|---|---|
| Eelgrass meadow | A dense tuft of flat ribbon blades, all leaning one way as if in current; visually the existing grass tuft laid over | 0.3–0.9 | `host: grass` — nearest shipped is `meadow-grass` |
| Turtlegrass | Wider, blunter, stiffer straps than eelgrass, sparser and taller | 0.3–0.6 | `host: grass` |
| Giant kelp | A long strap stipe rising to a surface canopy, with paired blades along its whole length and a gas bladder at the base of each blade. The bladder is the identifying feature and it is 2–5 cm | 20–40 | `host: reed` + `gen: kelp` — the reed generator makes tall near-vertical stems, but not a canopy that floats at a surface |
| Bull kelp | A single bare whip stipe to one large float, with a crown of blades trailing from it — a very strong and very simple silhouette | 10–25 | `host: reed` + `gen: kelp` |
| Sugar kelp | One broad crinkle-edged strap from a small holdfast, no branching | 1–3 | `host: reed` |
| Sea fan / gorgonian | A flat rigid net held perpendicular to the current — a plane, not a volume. Mesh openings are 1–3 cm and the mesh *is* the species | 0.5–1.5 | `gen: coral` ⚠ |
| Bladderwrack | Forking olive-brown straps with paired round bladders at each fork | 0.3–0.9 | `host: reed` |

⚠ **Sea fan.** A gorgonian's whole identity is a net of 1–3 cm openings. At a
5 cm lattice the net fills in solid and the animal becomes a paddle; at 1 cm it
works but the asset is a flat plane one voxel thick, which is the shape the fish
generator's fin plates already produce. Build it as a fin plate before building
it as a coral.

## Bushes / shrubs

**Not hosted, and there is no honest sea equivalent.** Soft corals and sponges
occupy the visual role of a shrub on a reef; they are listed with the corals
above rather than duplicated here.

## Birds

Hosted. This is one of only two biomes where birds are hosted and almost nothing
else is (`biomes.py:45-50` says exactly this — bare rock is the other).
Everything here is authored at 1 cm, which is shipped practice for every bird in
the library.

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Herring gull | Heavy white body, pale grey mantle, black wingtips with white spots, yellow bill with a red gonys spot | 0.60 | 1 cm | `shipped: herring-gull` |
| Northern gannet | Cigar-shaped brilliant white body, black wingtips, buff head, long dagger bill and a very long narrow wing; the silhouette is a cross | 0.9 | 1 cm | `queued` |
| Wandering albatross | The longest wing in the list — span roughly 3 m on a 1.2 m body, so the spread pose is almost all wing. White body, dark upperwing, heavy pink bill | 1.2 | 1 cm | `queued` |
| Northern fulmar | Gull-shaped but stiff-winged and bull-necked, grey and white, with a tube-nosed bill — the tube is 1 cm and will not read | 0.5 | 1 cm | `queued` |
| Manx shearwater | Black above, white below, narrow stiff wings held straight out; identity is the black/white split down the exact midline | 0.35 | 1 cm | `queued` |
| Atlantic puffin | Squat black-and-white body, orange feet, and a deep triangular bill in orange, blue-grey and yellow bands. The bill is a third of head length and is the entire species | 0.3 | 1 cm | `queued` |
| Common guillemot | Upright penguin-like posture, dark chocolate above, white below, thin pointed bill | 0.42 | 1 cm | `queued` |
| Razorbill | As guillemot but jet black with a deep blunt bill crossed by one white line | 0.4 | 1 cm | `queued` |
| Black-legged kittiwake | Small clean gull, grey mantle, wingtips dipped in solid black with no white spots, black legs | 0.4 | 1 cm | `queued` |
| Arctic tern | Very slight body, long forked tail streamers, black cap, blood-red bill and legs | 0.35 | 1 cm | `queued` |
| Sooty tern | All-dark above, white below, white forehead triangle, deeply forked tail | 0.44 | 1 cm | `queued` |
| Great skua | Bulky brown pirate-gull with a heavy hooked bill and a bold white flash at the base of the primaries | 0.55 | 1 cm | `queued` |
| Great cormorant | Long low-slung body, snake neck, hooked bill, oily blue-black; the wings-spread drying pose is the recognisable one | 0.9 | 1 cm | `queued` |
| Brown pelican | Huge pouched bill nearly as long as the body, heavy grey-brown body, short neck folded back in flight | 1.2 | 1 cm | `queued` |
| Magnificent frigatebird | All-black, deeply forked scissor tail, extremely long angled wings, and a red throat pouch on the male only — a dimorphism the bird generator's `_sex_scale` machinery already has a home for | 1.0 | 1 cm | `queued` |
| Wilson's storm petrel | Tiny, sooty, white rump band, feet trailing past the tail. At 18 cm this is below the owner's 20 cm floor and would be authored up, like four birds already in the library | 0.18 | 1 cm | `queued` ⚠ |
| Brown booby | Chocolate above with a hard-edged white belly, long wedge tail, pale dagger bill | 0.75 | 1 cm | `queued` |
| Black-browed albatross | Half the wandering albatross, white head with a dark brow smudge over the eye and a bright orange bill | 0.9 | 1 cm | `queued` |

⚠ **Storm petrel.** 18 cm at 1 cm is eighteen voxels, and four birds already in
this library (`european-robin`, `great-tit`, `common-kingfisher`, `barn-swallow`)
were authored above life size to clear a 20 cm floor, each saying so in its own
`notes`. Do the same here and write the reason down, or the next person will
"correct" it back.

## Land animals

**Not hosted, and mostly correct.** But three groups genuinely live in open
water and are not fish, not cetaceans and not birds, so they have nowhere to go
in the current kind list:

| Species | Voxel-artist description | Size (m) | Lattice | Status |
|---|---|---|---|---|
| Grey seal | Torpedo body, long straight roman-nosed muzzle, small foreflippers set well forward, hind flippers fused into a fan; blotched grey | 2.3 | 5 cm | `gen: pinniped` |
| Harbour seal | Shorter and rounder than a grey seal with a concave dog-like face; the muzzle profile is the only thing that separates them | 1.7 | 5 cm | `gen: pinniped` |
| Californian sea lion | Longer neck, visible external ear flaps, and foreflippers it can walk on; a much more upright animal than a true seal | 2.2 | 5 cm | `gen: pinniped` |
| Walrus | Enormous barrel body, blunt whiskered muzzle and two long downward tusks that are the entire silhouette | 3.2 | 5 cm | `gen: pinniped` |
| Green sea turtle | Flat oval carapace, paddle foreflippers longer than the shell is wide, small blunt head on a short neck | 1.1 shell | 2 cm | `gen: chelonian` |
| Leatherback turtle | Larger, with seven raised ridges running the length of a leathery keeled shell — the ridges are the species and they are 3–5 cm apart | 1.8 shell | 2 cm | `gen: chelonian` ⚠ |
| Common octopus | A sack mantle and eight tapering arms; no rigid axis anywhere, which is the reason no generator here can make it | 0.6 (1.3 with arms) | 2 cm | `gen: cephalopod` |
| Giant squid | Long tapered mantle with two terminal fins, eight arms and two much longer tentacles | 6 (mantle 2) | 5 cm | `gen: cephalopod` |
| Moon jellyfish | A translucent hemispherical bell with four horseshoe marks and a short trailing fringe | 0.3 bell | 1 cm | `gen: cephalopod` |
| Edible crab | Wide pie-crust-edged oval carapace, two heavy black-tipped claws, eight walking legs | 0.25 across | 1 cm | `gen: arthropod` |

⚠ **Leatherback ridges.** Seven ridges across a 1.8 m shell puts them roughly
20 cm apart in *span* but only 3–5 cm in *relief*. Relief is what a voxel lattice
resolves, so at 5 cm the shell is smooth and the animal becomes a green turtle.
2 cm holds it; that is the number to author at.

## Fish

Hosted, generator exists, and this is the deepest category in the world. Seven
cetaceans and nine ocean fish are already shipped.

Shipped practice for lattice: small fish 1 cm, medium 2 cm, sharks and dolphins
5 cm, great whales 10 cm.

### Shipped

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Bluefin tuna | Deep fusiform body, lunate tail, small finlets down to the wrist, steel blue above and silver below | 2.2 | 2 cm | `shipped: bluefin-tuna` |
| Whale shark | Broad flat head, checkerboard of pale spots and lines on dark grey, three ridges along each flank | 11.1 | 5 cm | `shipped: whale-shark` |
| Great white shark | Heavy conical snout, hard grey/white countershade line, large triangular first dorsal | 4.5 | 5 cm | `shipped: great-white-shark` |
| Tiger shark | Blunt square snout, faint vertical bars fading with age, long upper tail lobe | 4.0 | 5 cm | `shipped: tiger-shark` |
| Scalloped hammerhead | The head is the species: a wide scalloped hammer with the eyes at the tips | 3.5 | 5 cm | `shipped: scalloped-hammerhead` |
| Reef shark | Compact grey requiem shark with dark or white fin tips | 1.6 | 2 cm | `shipped: reef-shark` |
| Reef tang | Disc body, tiny mouth, single continuous dorsal, one bright accent | 0.22 | 1 cm | `shipped: reef-tang` |
| Clown anemonefish | Orange with three white vertical bars, black-edged. **Authored at 22 cm against a real ~10 cm** because ten voxels cannot hold three bars two voxels wide | 0.22 | 1 cm | `shipped: clown-anemonefish` |
| Shoal herring | Slim silver fusiform, deeply forked tail, no marking but countershading | 0.20 | 1 cm | `shipped: shoal-herring` |
| Blue whale | Long slate-blue body mottled paler, tiny far-aft dorsal, very broad flat rostrum | 25 | 10 cm | `shipped: blue-whale` |
| Sperm whale | Enormous square box head a third of total length, wrinkled flank, knuckled dorsal ridge | 13.3 | 10 cm | `shipped: sperm-whale` |
| Humpback whale | Knobbed rostrum and very long white flippers a third of body length | 14 | 10 cm | `shipped: humpback-whale` |
| Orca | Black above, white below with a white eye patch and a grey saddle; tall triangular dorsal on males | 7.0 | 5 cm | `shipped: orca` |
| Beluga | All white, no dorsal fin at all, bulbous melon forehead | 4.5 | 5 cm | `shipped: beluga` |
| Bottlenose dolphin | Grey, short thick beak, tall falcate dorsal | 3.0 | 5 cm | `shipped: bottlenose-dolphin` |
| Common dolphin | Slimmer, with a yellow-buff hourglass on each flank — the hourglass is the species | 2.4 | 2 cm | `shipped: common-dolphin` |

### Queued — open water and pelagic

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Atlantic cod | Thick-bodied, three separate dorsal fins, a chin barbel, and a pale lateral line curving over the pectoral | 1.0 | 2 cm | `queued` |
| Atlantic mackerel | Small streamlined fusiform with black wavy bars over an iridescent green back — bars run *over* the top, not down the flank | 0.4 | 1 cm | `queued` |
| European sea bass | Clean silver fusiform, two dorsals, the first spiny; sharp gill-cover edge | 0.7 | 2 cm | `queued` |
| Sardine | Small, round-bellied, single soft dorsal, one row of faint dark spots | 0.2 | 1 cm | `queued` |
| European anchovy | Very slim, huge underslung jaw, one broad silver flank stripe | 0.15 | 1 cm | `queued` ⚠ authored-up |
| Yellowfin tuna | Fusiform with extremely long sickle second dorsal and anal fins trailing back — those two fins are the species | 1.8 | 2 cm | `queued` |
| Atlantic sailfish | The dorsal is the species: a full-length fan taller than the body is deep, plus a spear bill | 2.5 | 5 cm | `queued` |
| Swordfish | Flat broad bill a third of body length, single tall crescent dorsal, no pelvic fins | 3.0 | 5 cm | `queued` |
| Blue marlin | Round-sectioned spear bill, short pointed dorsal, cobalt above with faint pale vertical bars | 3.5 | 5 cm | `queued` |
| Mahi-mahi | Blunt vertical forehead on males, one dorsal running nose to tail, green-gold flank | 1.2 | 2 cm | `queued` |
| Great barracuda | Long sagittiform silver body, underslung jaw of visible teeth, two widely separated dorsals, dark blotches low on the flank | 1.4 | 2 cm | `queued` |
| Giant trevally | Deep blunt-headed silver slab with a steep forehead and a scutted tail base | 1.0 | 2 cm | `queued` |
| Ocean sunfish | A disc with no tail — the body ends in a rudder fringe, with one tall dorsal and one tall anal fin opposite | 2.0 | 5 cm | `queued` |
| Flying fish | Slim body with pectoral fins as long as the body, held out as wings | 0.3 | 1 cm | `queued` |
| Oarfish | Ribbon body 15–20× longer than deep, silver, with a red crest of the first dorsal rays over the head | 5.0 | 5 cm | `queued` ⚠ |
| Basking shark | Slow grey shark with the mouth held wide open as a hoop wider than the head is deep | 8.0 | 5 cm | `queued` |
| Blue shark | The slimmest shark shape available: very long pectorals, long conical snout, deep indigo above | 2.5 | 5 cm | `queued` |
| Shortfin mako | Fast, compact, near-lunate tail, sharply pointed snout, brilliant blue | 2.8 | 5 cm | `queued` |
| Thresher shark | The upper tail lobe is as long as the whole body — half the asset is tail | 4.5 | 5 cm | `queued` |
| Nurse shark | Bottom-hugging, blunt rounded head, two barbels, two dorsals set far back | 2.5 | 5 cm | `queued` |
| Sand tiger shark | Ragged protruding teeth visible with the mouth closed, hunched back, rusty spots | 2.5 | 5 cm | `queued` |
| Giant manta ray | A flat diamond wing 5–7 m across with two forward cephalic lobes and a whip tail; nothing else in the library is this shape | 5.0 span | 5 cm | `queued` |
| Spotted eagle ray | Narrower diamond, long duck-like snout, dark back covered in even white spots | 2.0 span | 2 cm | `queued` |
| Common stingray | Rounded disc, no wing tips to speak of, a single serrated spine on a slender tail | 0.9 span | 2 cm | `queued` |
| Atlantic halibut | Huge flat diamond flatfish, both eyes on one side, dark above and pure white below | 2.0 | 5 cm | `queued` |
| European plaice | Small brown flatfish with a scatter of orange spots — the spots are the species | 0.4 | 1 cm | `queued` |
| Turbot | Almost circular flatfish, no scales, bony tubercles over the upper side | 0.6 | 2 cm | `queued` |
| Conger eel | Very heavy anguilliform, dorsal starting just behind the pectoral, grey-brown | 2.0 | 2 cm | `queued` |
| Atlantic wolffish | Blunt-headed, thick-lipped eel-like body with visible canines and dark vertical bars | 1.2 | 2 cm | `queued` |
| Anglerfish | Globiform, enormous upturned mouth, and a single illicium rod over the snout with a lure at its tip. The lure is 2–3 cm and is the entire species | 0.8 | 1 cm | `queued` ⚠ |

### Queued — reef

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Parrotfish | Deep body, fused beak-like jaws, blunt forehead, blue-green with a pink cheek streak | 0.5 | 1 cm | `queued` |
| Moorish idol | A near-triangular disc with a long white filament trailing off the dorsal and three broad vertical bands | 0.2 | 1 cm | `queued` ⚠ authored-up |
| Emperor angelfish | Deep disc, tight diagonal blue-and-yellow stripes, a dark mask across the eye | 0.35 | 1 cm | `queued` |
| Racoon butterflyfish | Disc body, black eye band and a second dark bar behind the head | 0.2 | 1 cm | `queued` ⚠ authored-up |
| Lionfish | The fins are the species: eighteen long separated venomous spines fanned out around a red-and-white banded body | 0.35 | 1 cm | `queued` ⚠ |
| Porcupinefish | Globiform, inflated, covered in short erect spines, with very large eyes | 0.4 | 1 cm | `queued` |
| Yellow boxfish | A near-cube with tiny fins at the corners — the one fish whose silhouette is a rectangle | 0.25 | 1 cm | `queued` |
| Giant moray | Heavy anguilliform with no pectoral fins, a continuous fin ridge from head to tail, mottled dark, mouth held open | 2.0 | 2 cm | `queued` |
| Napoleon wrasse | Very large deep-bodied wrasse with a bulging forehead hump and thick lips | 1.5 | 2 cm | `queued` |
| Coral grouper | Heavy blunt-headed fusiform, big mouth, red with small blue spots | 0.6 | 2 cm | `queued` |
| Bluestripe snapper | Yellow with four hard-edged horizontal blue stripes — a textbook 2-on-2-off stripe test | 0.3 | 1 cm | `queued` |
| Clown triggerfish | Rounded body, tiny separate first dorsal spine, large white belly blotches on black | 0.4 | 1 cm | `queued` |
| Longsnout seahorse | Vertical S body, tubular snout, prehensile curled tail, bony ridges | 0.15 | 1 cm | `queued` ⚠ |
| Garden eel | A thin vertical stalk emerging from sand, only the upper third of the animal visible | 0.4 (0.15 visible) | 1 cm | `queued` |
| Remora | Slim, with an oval ridged sucker plate on top of the head; only reads when attached to something else | 0.5 | 1 cm | `queued` |

### Queued — cetaceans

| Species | Voxel-artist description | Length (m) | Lattice | Status |
|---|---|---|---|---|
| Fin whale | Long, sleek, with an asymmetric jaw — white on the right side only, dark on the left. That asymmetry is the species and it is free at any lattice | 20 | 10 cm | `queued` |
| Minke whale | Smallest rorqual, sharply pointed rostrum, a clean white band across each flipper | 8 | 5 cm | `queued` |
| Sei whale | Between minke and fin, single ridge on the rostrum, tall falcate dorsal set well forward of the others | 15 | 10 cm | `queued` |
| Grey whale | Mottled grey, no dorsal fin — a low knuckled ridge instead — and heavy barnacle patches on the head | 13 | 10 cm | `queued` |
| North Atlantic right whale | No dorsal fin, a strongly arched jawline, and rough pale callosity patches on the head | 15 | 10 cm | `queued` |
| Narwhal | Mottled grey-white, no dorsal fin, and a single straight spiral tusk on males up to 2.5 m — the tusk is half the asset | 4.5 (+2.5 tusk) | 5 cm | `queued` |
| Long-finned pilot whale | Bulbous melon, very broad low-set dorsal, all black with a pale anchor patch on the throat | 5.5 | 5 cm | `queued` |
| Harbour porpoise | Small, blunt-faced with no beak at all, small triangular dorsal | 1.6 | 2 cm | `queued` |
| Risso's dolphin | Blunt vertical forehead with a crease, tall dorsal, and a body that scars pale white with age — an old one is nearly white | 3.5 | 5 cm | `queued` |
| Spinner dolphin | Slim three-tone flank — dark cape, grey side, white belly — and a long thin beak | 2.0 | 2 cm | `queued` |

⚠ **Four lattice notes for this section.**

* **Anchovy, Moorish idol, racoon butterflyfish, seahorse.** All 15–20 cm, which
  is 15–20 voxels at 1 cm. The Moorish idol needs three bands, the butterflyfish
  two, and the fish research's own floor is 2 voxels on, 2 voxels off. Author
  these above life size the way `clown-anemonefish` was, and say so in `notes`.
* **Lionfish.** Eighteen separated spines on a 35 cm body means a spine every
  ~2 cm with a ~1 cm gap. At 1 cm the gaps close and the fan becomes a paddle.
  Either author fewer, thicker spines and accept it is a stylisation, or accept a
  paddle. Do not pretend the count is achievable.
* **Oarfish.** A 15–20:1 length-to-depth ratio at 5 cm gives a body 5–6 voxels
  deep and 100 long. The fish research already recorded that an eel's caudal
  peduncle rounds to 0.2 voxels and that the body axis has to be stamped as a
  solid one-voxel run first. This species is the extreme case of that.
* **Anglerfish lure.** A 2–3 cm bulb on a 1 cm lattice is two or three voxels,
  which just clears the floor — but only if the rod holding it is drawn at least
  one voxel thick along its whole length, which means drawing the rod thicker
  than life. That is fine; write it down.

---

## Build priority

Ocean is the biome that pays back fastest per asset, because the water column
needs no engine change and the fish and bird generators both exist and are both
proven.

1. **Pelagic shoal fish first** — mackerel, sardine, cod, sea bass. They are the
   `fish` generator's core case (fusiform, one marking, countershaded), the
   schooling fields in the `detail` group already exist, and a shoal is many
   entities for one spec.
2. **Seabirds second.** Gannet, cormorant, tern, puffin, kittiwake. All 1 cm, all
   within the shipped bird generator, and they are what makes an empty sea read
   as inhabited from a boat or a shoreline.
3. **The remaining rorquals.** Fin, minke, grey, right. The cetacean generator is
   shipped and these are parameter changes on it, not new work. Fin whale in
   particular is free: its asymmetric jaw is a palette split.
4. **Rays and flatfish.** These are the one shape family the fish generator has
   never been asked for — a depressiform body that is wider than it is deep. Do
   one (spotted eagle ray) as a probe before committing to five.
5. **Then, and only then, the `hosts` change** to admit `rock` and the tuft kinds,
   which unlocks reefs and kelp. This is last not because it is unimportant — it
   is the biggest single gap in the biome — but because it is the only item that
   needs someone else's file changed, and the four above do not.

## Where the numbers come from

Every size in this file is an **approximate typical adult figure from general
knowledge**. None of it is measured, and none of it is sourced. It is good
enough to choose a voxel lattice, which is all this document is for, and it is
**not** good enough to quote as a fact or to paste into a spec's `notes` without
checking first.

The repo references — `biome.h` line numbers, `biomes.py` line numbers,
`kinds.py` line numbers, shipped spec names and their authored sizes — were read
out of the files and are exact.

Two specific hedges worth recording:

* **The lattice numbers in the Lattice column are recommendations derived from
  the three-voxel rule, not measurements.** Only the shipped rows have a lattice
  that has actually been tested. Every `queued` row's lattice should be confirmed
  with `tools/fishprobe.py --lattice` or the bird equivalent before the spec is
  written, exactly as the fish and bird research documents did for their sets.
* **The coral, kelp and pinniped generator verdicts are arguments, not
  findings.** Each says which existing generator I think comes closest and why;
  none has been tried.
