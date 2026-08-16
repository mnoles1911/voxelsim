# Aquatic rocks and plants — the species list

**164 entries: 101 saltwater and 63 freshwater**, covering the algae,
seagrasses, corals, sponges, anemones, submerged and floating freshwater plants,
and the sea-floor and river-bed rock that the ten per-biome master lists cover in
about fifteen rows between them.

**123 of them are built by this pass** and are listed `built`; 28 were already in
`specs/`; 13 are `blocked` and each one names the number that stopped it in §8.

This document supersedes and extends the aquatic rows in
[`biomes/00-ocean.md`](biomes/00-ocean.md) and the waterside rows scattered
through `biomes/01-beach.md`, `02-grassland.md`, `03-temperate-forest.md` and
`07-taiga.md`. Where a row here disagrees with one there, this one is later and
was written after the ocean `hosts` change; the biome files were written while
the sea floor still hosted nothing.

| Environment | Plants | Rocks | Total | Built here | Already shipped | Blocked |
|---|---|---|---|---|---|---|
| **Saltwater** | 62 | 39 | **101** | 71 | 22 | 8 |
| **Freshwater** | 47 | 16 | **63** | 52 | 6 | 5 |
| **Total** | **109** | **55** | **164** | **123** | **28** | **13** |

Thirteen entries are **not buildable** and say which number stopped them; they
are in §8. Everything else is either shipped or authorable with a generator that
exists today.

---

## 1. Why this document exists now

Until 2026-08-15 the Ocean biome's `hosts` tuple was `("fish", "cetacean",
"bird")`. Nothing that grows and no stone could stand on the sea floor, so
`00-ocean.md` marked fifteen rows `host: rock` / `host: grass` / `host: reed`
and stopped. The owner has opened it:

    biomes.BY_KEY['ocean'].hosts
      == ('fish', 'cetacean', 'bird', 'rock', 'grass', 'reed', 'bush')

`plantable` deliberately stays `False`, and that distinction is the point of the
row. **`plantable` means "a land plant can root here"**, which is still no.
**`hosts` means "an asset of this kind belongs here"**, which for kelp, seagrass
and a reef is plainly yes. (`forge/biomes.py:65-79`.)

**There is no freshwater biome and there is not going to be one.** The world has
exactly ten biomes and fresh water is not one of them: a river, a pond or a lake
sits *inside* grassland, temperate forest, taiga or tundra, and the engine
classifies the column by the land around the water (`forge/biomes.py:37-42`). So
a freshwater plant is authored with **weights on the land biomes whose water it
lives in**, exactly as the freshwater fish already are —
`specs/brown-trout.json` carries temperate forest 0.9, taiga 0.6, tundra 0.35,
grassland 0.3, read out of the file. Every freshwater row below follows that
convention and none of them touches `biomes.ocean`.

---

## 2. How to read the tables

**Kind** is the `kind` field the species would be authored as. Six are available
to water: `rock`, `grass`, `reed`, `bush`, `flower` and (for the animals that
are not in this document) `fish`. `flower` is *not* in the ocean `hosts` tuple —
see §8.

**Status** is one of:

* `shipped: <name>` — a spec exists in `specs/` today.
* `built` — authored by **this** pass; the seed script is named in §7.
* `queued` — a generator exists and the row is authoring work only.
* `blocked: <reason>` — §8 has the number that stopped it.

**Size (m)** is the plant's height above the bed, or the rock's `block_size_m`.
Rocks are **always 10 cm** (`forge/kinds.py:29-59`, enforced by
`forge.cli.selftest`); everything else picks its own lattice by the house rule
and the table says which.

**⚠** marks a species authored above its real size to clear the 20 cm floor, or
whose defining feature does not survive its lattice. The note under the table
carries the arithmetic.

---

## 3. Three decisions, with the measurements behind them

These are decisions, not details. Each one was measured on this machine rather
than argued.

### 3.1 Is coral a `rock` or a plant kind? — **Both, split by branch thickness, and the line is at about 10 cm.**

Coral is calcium carbonate. Structurally it is stone, it is coloured like
nothing in the rock palette, and several forms are shaped like a bush. The
question is not what it is made of; it is **whether the 10 cm terrain lattice
can hold the shape**, because a `rock` is a terrain-lattice asset fixed at 10 cm
and `forge.cli.selftest` refuses any other value.

Measured on `specs/branching-stony-coral.json` (a `bush`, authored 1.0 m, seed 1,
built at four lattices):

| Lattice | Voxels | Build | Branch tip diameter | Pieces |
|---|---|---|---|---|
| 1 cm | 32,055 | 378 ms | 5.00 vox | 1 |
| 2 cm | 5,302 | 184 ms | 2.50 vox | 1 |
| 5 cm | 1,010 | 138 ms | 1.00 vox | 1 |
| **10 cm** | **397** | 129 ms | **0.50 vox** | 1 |

**What the 10 cm lattice costs a staghorn: 92.5% of it.** 5,302 voxels at the
authored 2 cm become 397 at 10 cm. The tip diameter is the reason — a real
staghorn branch is roughly 2–5 cm through (*estimate, general knowledge*), which
at 10 cm is half a voxel, so it is drawn at the generator's one-voxel minimum:
**a 2.5 cm branch becomes a 10 cm branch, four times life size**, and the
thicket's negative space — the gaps that are most of what a staghorn *looks
like* — closes at the same rate. There is a second, independent wall in the same
place: `growth.kill_m` has a **0.10 m floor**, so no two branches in this library
can come closer than 10 cm at any lattice, which is already about twice a real
staghorn's branch spacing. Both were found by the pass that shipped
`branching-stony-coral`; the branch-diameter half is measured above.

So the split is by form, and it is honest rather than tidy:

| Coral form | Kind | Why |
|---|---|---|
| Brain, boulder-star, mushroom, lettuce, pillar, fire, organ-pipe, plate/table | **`rock`** | A dome, a disc, a lump or a fluted column. Its smallest identifying feature is decimetres or larger, so 10 cm holds it, and being terrain-lattice makes it destructible like the reef it is part of — which is *correct* for a reef and would be wrong for a plant. |
| Staghorn, elkhorn, branching stony, cold-water (Lophelia), black coral, soft corals | **`bush`** | The branch IS the species and it is centimetres. A detail-lattice bush picks 2 cm and keeps 5,302 voxels instead of 397. |
| Sea whip / sea rod | **`reed`** | One unbranched vertical whip from a holdfast, which is exactly what the tuft generator makes. |
| Sea fan / gorgonian | — | **Blocked.** §8.1. |

**The cost of being a `bush` is that it is not destructible as terrain**, which
for a reef head is a real loss and is why the massive forms are `rock`
deliberately rather than by default. A reef made of `bush` heads would be a reef
you cannot break.

### 3.2 How big can kelp be? — **28 m, at 5 cm, and the cost is not what the brief assumed.**

The arithmetic in the brief is right: a 30 m giant kelp at the 5 cm ground-cover
lattice is 600 voxels of stipe. The conclusion does not follow. Measured on
`specs/giant-kelp.json` (authored 28 m, seed 1 realises at **39.6 m**):

| Lattice | Voxels | Grid | Build |
|---|---|---|---|
| **5 cm (authored)** | **18,341** | 52.7 MB | 1,321 ms |
| 10 cm | 3,189 | 6.9 MB | 166 ms |
| 20 cm | 1,513 | 1.0 MB | 45 ms |

**18,341 voxels is not expensive.** One `temperate-oak` is 1,065,343
(`README.md`). The tallest plant in the library by eight times costs **1.7% of
one oak**, and the whole 30-species saltwater plant set built below comes to
less than a fifth of one. Kelp does not need a coarser lattice, a shorter
authored size or a different kind, and the 600-voxel stipe was never the
problem.

**What IS expensive is the bounding box, and that is a different fact with a
different fix.** 52.7 MB of dense grid holds 18,341 voxels: an occupancy of
**0.048%**, the emptiest asset in the library by a wide margin. A 40 m column
11 m across is 38 million cells of water. That is a storage-representation
observation for whoever writes the asset loader — it is not an argument about
the lattice, and dropping to 10 cm would trade 84% of the plant away to save
46 MB of transient allocation that `ASSET_FORGE_MAX_GRID_MB` (3 GB) does not
care about.

**Where 10 cm would actually cost something.** `giant-kelp` authors
`tuft.width_m` 0.10 — the stipe is 2 voxels through at 5 cm and **1 at 10 cm**,
the one-voxel minimum, at which a 40 m plant is a 40 m thread. The kelps built
below are 5 cm for that reason and no other.

**The honest limitation is not cost, it is that a kelp is not a tuft.** Giant
kelp carries paired blades along the *whole length* of the stipe, each with a
2–5 cm gas bladder at its base, and `tuft.head` only ever sits on the TOP of a
stem — there is no along-the-stem foliage anywhere in the tuft generator. So
what ships is a bare strap with one plume standing in for the surface canopy.
That is the read from below and it is not the structure of the plant. The
bladder is 2–5 cm (*estimate*) and is one voxel at 5 cm; the lattice that would
hold it is 1 cm, at which the plant is 2,800 voxels of stipe. It is not drawn at
any size and that is recorded, not hidden.

### 3.3 Nothing records depth — **and the shape that would has been sitting in `spec.py` all along, scoped to animals.**

A submerged plant, a floating-leaved plant and an emergent one are three
different depths of water and **no parameter in a plant spec says so.** Measured:

* **`detail.depth_min_m`, `detail.depth_max_m` and `detail.min_water_depth_m`
  already exist and already mean exactly the right thing** — but
  `spec.PARAMS` scopes all three to `kinds=('fish', 'cetacean')`, so the app
  never shows them for a `grass` or a `rock` and nothing documents them as
  meaning anything for a plant.
* **They are nevertheless writable today, silently.** Patching a `grass` spec
  with `detail.depth_min_m: 15.0` **succeeds with no warning** — the `kinds`
  tuple gates the *UI*, not `validate`. That is worse than a refusal, and it is
  this project's signature failure shape: **every one of the 705 specs on disk
  already carries `detail.depth_min_m: 0.3`** because the canonical spec is
  full-schema, so an authored 15.0 on a kelp is indistinguishable from the 0.3 a
  `saguaro` is carrying. There is no "unset" for a consumer to test against.
* **`placement.elev_min_m` bottoms out at −10.0 m.** Measured: patching −30.0
  clamps to −10.0 and warns `placement.elev_min_m: -30.0 clamped to [-10.0,
  4000.0]`. A kelp forest at 20–40 m down cannot be expressed, and every ocean
  spec in the library is pinned at the floor.
* **`placement.water_max_m` is not depth and is meaningless here.** It is
  distance to a watercourse — for riverbank willows — and 0 means "does not
  care". Every aquatic spec in the library carries 0.

**What would be needed**, if it is wanted: one new `placement` sub-group of
three floats — a water-depth minimum, a water-depth maximum, and a habit
enumeration (`submerged` / `floating` / `emergent` / `intertidal`) — declared
with `kinds` covering the plant and rock kinds, plus lowering the
`placement.elev_min_m` floor from −10 m to something like −200 m so the sea floor
is addressable at all. The habit enumeration is the one that carries the most
per byte, because it is the thing a scatterer actually branches on and it cannot
be inferred from two depths.

**Not added.** `placement.*` is read by no code yet and its shape is the owner's
to set. Every aquatic spec below instead states its depth band **in its own
`notes`**, in words, which is a comment and not a contract and is labelled as
such — the alternative was writing numbers into `detail.depth_*` that would look
authored and be indistinguishable from a default.

---

## 4. Saltwater

### 4.1 Kelps and large brown algae

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Giant kelp | A long bare strap stipe rising 20–40 m to a surface canopy; paired blades the whole length, each with a gas bladder at its base | 28 | Ocean | `reed` | 5 cm | `shipped: giant-kelp` ⚠ |
| Bull kelp | One bare whip stipe to a single large float, with a crown of blades trailing from it — a very strong, very simple silhouette | 14 | Ocean | `reed` | 5 cm | `shipped: bull-kelp` |
| Sugar kelp | One broad crinkle-edged strap from a small holdfast; no branching at all | 2.2 | Ocean, Beach | `reed` | 5 cm | `shipped: sugar-kelp` |
| Oarweed | A short stout stipe carrying a blade split into a hand of finger straps — the split is the species | 1.6 | Ocean, Beach | `reed` | 5 cm | `built` |
| Dabberlocks | A long strap with a thick pale midrib running its whole length and frilly wings either side | 2.4 | Ocean | `reed` | 5 cm | `built` |
| Furbelows | A huge stiff blade on a flattened twisted stipe rising from a warty bulbous holdfast the size of a football | 2.8 | Ocean | `reed` | 5 cm | `built` |
| Sea palm | A stout upright trunk barely half a metre high with a drooping mop of straps — a palm tree in the surf zone | 0.55 | Ocean, Beach | `reed` | 5 cm | `built` |
| Thongweed | A ring of small buttons on the rock, each throwing one very long forking bootlace | 1.8 | Ocean, Beach | `reed` | 5 cm | `built` |
| Feather-boa kelp | One very long flattened midrib strap carrying short blades and bladders down both sides | 3.6 | Ocean | `reed` | 5 cm | `built` |
| Knotted wrack | Long olive straps with one large egg-shaped bladder set into the strap at intervals, no midrib | 1.0 | Beach, Ocean | `reed` | 5 cm | `built` |
| Bladderwrack | Forking olive-brown straps with paired round bladders at each fork | 0.6 | Beach, Ocean | `reed` | 5 cm | `shipped: bladderwrack` |
| Serrated wrack | Flat forking straps with a saw-toothed edge and no bladders at all — the flattest, limpest wrack | 0.45 | Beach, Ocean | `reed` | 5 cm | `built` |
| Spiral wrack | Short forking straps twisted along their length, upper-shore, drying to olive-black | 0.3 | Beach | `reed` | 5 cm | `built` |
| Channelled wrack | Very short stiff straps curled into a gutter down one side; the topmost seaweed on any shore | 0.22 | Beach | `reed` | 5 cm | `built` ⚠ |
| Sargassum weed | Bushy and branching rather than strappy, with berry-sized round bladders scattered through it | 0.8 | Ocean | `bush` | 5 cm | `built` |
| Sea oak (Halidrys) | A stiff zig-zag branching bush with pod-shaped bladders at the tips | 0.6 | Ocean | `bush` | 5 cm | `built` |

⚠ **Giant kelp** — §3.2. The bladders and the along-stipe blades are not drawn
at any lattice.

⚠ **Channelled wrack** is authored at 0.22 m against a real 0.10–0.15 m
(*estimate*), for the 20 cm floor. Real ratio ~1.7×, which is below
`clown-anemonefish`'s shipped 2.2×.

### 4.2 Red algae

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Dulse | Flat deep-red hand-shaped fronds splitting into broad lobes from one short stalk | 0.4 | Ocean, Beach | `grass` | 5 cm | `built` |
| Irish moss | A dense low fan of flat forking blades, iridescent blue at the tips in sunlight | 0.25 | Beach, Ocean | `grass` | 5 cm | `built` ⚠ |
| Coralline turf | A stiff pink mat of jointed calcified branches — snaps rather than bends | 0.22 | Ocean, Beach | `grass` | 5 cm | `built` ⚠ |
| Laver (nori) | Thin translucent purple-brown sheets clinging flat, like wet tissue paper | 0.3 | Beach | `grass` | 5 cm | `built` |
| Red comb-weed (Plocamium) | Small, bright red, finely branched with all the branchlets combed to one side | 0.25 | Ocean | `grass` | 5 cm | `built` ⚠ |
| Maerl nodule | A loose unattached pink coralline knuckle a few centimetres across | 0.06 | Ocean | — | — | `blocked: below floor` — the **bed** is a rock, §4.8 |

⚠ **Coralline turf** authored 0.22 m against a real 0.05–0.10 m (*estimate*).
That is 2.2–4.4×, at the high end of house practice; it is here because the
alternative is a habitat with no ground layer at all.

⚠ **Irish moss and red comb-weed are `grass` and both were `bush` first.** §8.8.

### 4.3 Green algae

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Sea lettuce | Broad crumpled sheets of brilliant translucent green, two cells thick, tearing easily | 0.3 | Beach, Ocean | `grass` | 5 cm | `built` |
| Gutweed | Bright green inflated tubes, unbranched, gas-filled so they stand up in a pool | 0.25 | Beach | `grass` | 5 cm | `built` ⚠ |
| Green sea fingers | Spongy dark-green forking cylindrical fingers, velvety, thicker than they look | 0.35 | Ocean, Beach | `grass` | 5 cm | `built` ⚠ |
| Halimeda | Chains of small flat calcified discs jointed end to end — a plant that makes sand | 0.25 | Ocean | `grass` | 5 cm | `built` ⚠ |
| Mermaid's wineglass | A single stalk topped by one tiny green parasol | 0.05 | Ocean | — | — | `blocked: below floor` |

⚠ **Gutweed** authored 0.25 m against a real 0.10–0.30 m (*estimate*) — within
range at the top end, not an authored-up.

⚠ **Green sea fingers and halimeda are `grass` and both were `bush` first.**
§8.8. Codium loses least by it: its fingers are genuinely round cylinders and a
tuft stem is a swept capsule, so the primitive is exactly right.

### 4.4 Seagrasses — the only flowering plants that live fully submerged in the sea

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Eelgrass | Dense tuft of flat ribbon blades, all leaning one way as if in current | 0.6 | Ocean, Beach | `grass` | 5 cm | `shipped: eelgrass-meadow` |
| Turtlegrass | Wider, blunter, stiffer straps than eelgrass; sparser and taller | 0.5 | Ocean, Beach | `grass` | 5 cm | `shipped: turtlegrass` |
| Neptune grass | The largest seagrass: long stiff ribbons over a thick fibrous mat of old leaf bases | 0.9 | Ocean | `grass` | 5 cm | `built` |
| Manatee grass | Leaves are **cylindrical**, not flat — the only round-leaved seagrass, so it reads as a tuft of green wires | 0.4 | Ocean | `grass` | 5 cm | `built` |
| Shoal grass | Very narrow flat blades with a notched tip; the pioneer that colonises bare sand first | 0.25 | Ocean, Beach | `grass` | 5 cm | `built` |
| Surfgrass | Long bright-green ribbons growing on **rock** in the surf, streaming with every wave | 0.8 | Ocean, Beach | `grass` | 5 cm | `built` |
| Dwarf eelgrass | A short fine intertidal turf on mudflats, exposed at low tide | 0.22 | Beach, Ocean | `grass` | 5 cm | `built` ⚠ |

⚠ **Dwarf eelgrass** authored 0.22 m against a real 0.10–0.20 m (*estimate*).

### 4.5 Corals — see §3.1 for why each is the kind it is

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Brain coral | A boulder-shaped dome with a meandering surface groove maze | 1.6 | Ocean | `rock` | 10 cm | `shipped: brain-coral` ⚠ |
| Plate / table coral | One flat horizontal disc on a short central stem, a mushroom cap in stone | 2.0 | Ocean | `rock` | 10 cm | `shipped: plate-coral` |
| Branching stony coral | A dense thicket of finger-thick branches on a hemispherical base | 1.0 | Ocean | `bush` | 2 cm | `shipped: branching-stony-coral` ⚠ |
| Staghorn coral | Open antler branching, few thick tines, cream with pale growing tips | 1.2 | Ocean | `bush` | 2 cm | `built` ⚠ |
| Elkhorn coral | Broad flattened blade-branches like a moose's palm, oriented into the current | 1.4 | Ocean | `bush` | 2 cm | `built` |
| Pillar coral | A cluster of blunt vertical fingers rising off a shared encrusting base | 1.5 | Ocean | `rock` | 10 cm | `built` |
| Boulder star coral | A lumpy hemispherical mound covered in low knobs, the reef's bulk builder | 1.8 | Ocean | `rock` | 10 cm | `built` |
| Mushroom coral | One free-living unattached disc with radial ribs, sitting loose on sand | 0.25 | Ocean | — | — | `blocked: lattice`, §8.7 |
| Lettuce coral | Whorled crinkled plates stacked in a rosette, like a cabbage in stone | 0.9 | Ocean | `rock` | 10 cm | `built` |
| Fire coral | Flat mustard-yellow upright blades with smooth white edges, no visible polyps | 0.7 | Ocean | `rock` | 10 cm | `built` |
| Organ-pipe coral | Tight parallel red vertical tubes with flat tops, like a bundle of straws | 0.4 | Ocean | `rock` | 10 cm | `built` |
| Bubble coral | A dome covered in fat rounded translucent vesicles the size of grapes | 0.4 | Ocean | `rock` | 10 cm | `built` |
| Cold-water coral (Lophelia) | Brilliant white open branching thickets, deep and cold, no algae in it at all | 1.0 | Ocean | `bush` | 2 cm | `built` |
| Black coral tree | A tall dark feathery tree-shaped colony, finely branched, deep water | 2.0 | Ocean | `bush` | 2 cm | `built` |
| Dead man's fingers | Fat blunt lobed soft fingers rising from an encrusting base, white to orange | 0.25 | Ocean | `grass` | 5 cm | `built` ⚠ |
| Carnation soft coral | A drooping bunch of translucent pink stalks with flower-like polyp clusters | 0.4 | Ocean | `bush` | 2 cm | `built` |
| Leather coral | A thick fleshy stalked toadstool with a folded rubbery cap | 0.5 | Ocean | `bush` | 2 cm | `built` |
| Sea whip / sea rod | One or a few unbranched red or yellow whips standing straight off the bottom | 1.0 | Ocean | `reed` | 5 cm | `built` |
| Sea fan / gorgonian | A flat rigid net held across the current — a plane, not a volume | 1.0 | Ocean | — | — | `blocked: placement`, §8.1 |
| Coralline algal crust | A pink-purple encrusting film over rock, millimetres thick | — | Ocean | — | — | `blocked: material`, §8.5 |

⚠ **Brain coral.** Its meandering grooves are 5–10 mm (*estimate*), a tenth of a
voxel at the 10 cm a rock is locked to and not rescuable by a lattice change.
`rock.flutes` gives a grooved surface, not a maze. Recorded in that spec's own
`notes`.

⚠ **Branching stony coral and staghorn.** §3.1. `growth.kill_m`'s 0.10 m floor
holds branches about twice as far apart as life.

⚠ **Mushroom coral is not built.** §8.7 has the size-against-flatten sweep that
killed it: at the terrain lattice the whole organism is under three voxels
across.

⚠ **Dead man's fingers is a `grass` at 5 cm and was a `bush` at 2 cm.** It is
the one of the five that did not merely look thin — `pipeline.health` rejected
it outright at seed 2 with *"bare: the trunk never branched"*. §8.8.

### 4.6 Sponges

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Giant barrel sponge | A metre-wide rough-walled barrel with a deep central well; the oldest animal on a reef | 1.4 | Ocean | `rock` | 10 cm | `built` ⚠ |
| Tube sponge cluster | A bundle of tall thin-walled vertical tubes in purple or blue, splayed slightly | 1.0 | Ocean | `rock` | 10 cm | `built` |
| Elephant-ear sponge | A broad ruffled orange fan standing on edge off the reef wall | 1.0 | Ocean | `rock` | 10 cm | `built` |
| Vase sponge | One deep flaring cup with a rough knobbly outer wall | 0.45 | Ocean | `rock` | 10 cm | `built` |
| Encrusting sponge | A brightly coloured film a few millimetres thick over rock | — | Ocean | — | — | `blocked: material`, §8.5 |

⚠ **Barrel sponge.** The generator only cuts, and `rock.pans` dishes a top
surface rather than boring a well — see §8.4. What ships is a barrel with a
hollowed top, not a tube.

### 4.7 Anemones and other soft-bodied sessile animals

Filed with the plants because they occupy a plant's visual role on the sea floor
and are built by the plant generators, not because they are plants.

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Snakelocks anemone | A dense mop of long wavy grass-green tentacles with magenta tips; never retracts | 0.3 | Ocean, Beach | `grass` | 5 cm | `built` |
| Magnificent sea anemone | A wide carpet of thick blunt tentacles over a coloured column — the clownfish host | 0.7 | Ocean | `grass` | 5 cm | `built` |
| Giant green anemone | A short thick column with a flat crown of stubby tentacles in a ring | 0.3 | Ocean, Beach | `grass` | 5 cm | `built` |
| Plumose anemone | A tall smooth column topped by a dense white frilly crown, like a cauliflower on a stalk | 0.4 | Ocean | `flower` | 5 cm | `built` ⚠ |
| Tube anemone | Very long fine tentacles in two rings, rising from a leathery tube buried in mud | 0.45 | Ocean | `grass` | 5 cm | `built` |
| Sea pen | A single quill stuck upright in mud, feathered down both sides | 0.5 | Ocean | `reed` | 5 cm | `built` |
| Feather-duster worm | A round crown of fine banded filaments on a leathery tube, snapping shut when touched | 0.25 | Ocean, Beach | `grass` | 5 cm | `built` |
| Beadlet anemone | A blob of dark red jelly when closed, a small ring of tentacles when open | 0.05 | Beach | — | — | `blocked: below floor` — 4× would be needed |

⚠ **Plumose anemone is a `flower`, and `flower` is NOT in ocean's `hosts`
tuple.** See §8.2. It is authored with a **Beach** weight only and an ocean
weight of 0, which is a real place for it (harbour walls and pilings) and is
half the species' range.

### 4.8 Sea-floor rock, reef structure and rubble — all `rock`, all 10 cm

| Species | Voxel-artist description | Size (m) | Biomes | Status |
|---|---|---|---|---|
| Submerged granite boulder | A rounded joint block part-buried in mud rather than soil | 1.6 | Ocean, Beach | `shipped: submerged-granite-boulder` |
| Boulder reef | A loose cluster of angular blocks with wide gaps, sitting proud of the bottom | 1.8 | Ocean, Beach | `shipped: boulder-reef` |
| Bedrock ledge | A flat-topped shelf stepping down in risers, undercut at the base | 3.0 | Ocean, Beach | `shipped: bedrock-ledge` |
| Seamount flank | One large steep-sided cone section, no burial, sharp facets | 6.0 | Ocean | `shipped: seamount-flank` |
| Sea cave mouth | A dark arched void cut into a ledge face | 4.0 | Ocean, Beach | `shipped: sea-cave-mouth` |
| Rubble apron | Fine angular scree fanning out from a ledge foot | 0.6 | Ocean, Beach, Bare rock | `shipped: rubble-apron` |
| Reef spur | A long finger of reef running seaward with a sand groove either side; the spur-and-groove that faces every surf coast | 3.2 | Ocean | `built` |
| Patch-reef head | An isolated mound of consolidated reef rock standing alone on sand, undercut at the foot | 2.6 | Ocean | `built` |
| Reef-flat pavement | A near-flat consolidated limestone floor, pitted and pocked, barely proud of the sand | 3.0 | Ocean, Beach | `built` |
| Coral rubble bank | A heap of broken coral sticks and plates, sharp-edged and unsorted, thrown up behind a reef crest | 1.4 | Ocean, Beach | `built` |
| Maerl bed | A low lens of loose pink coralline knuckles, every one a few centimetres across | 1.2 | Ocean | `built` ⚠ |
| Oyster reef bank | A ridge of cemented shell, every surface made of overlapping curved plates | 1.6 | Ocean, Beach | `built` |
| Honeycomb worm reef | A biogenic sandstone crust of packed tubes, the surface a mass of small round holes | 1.2 | Beach, Ocean | `built` ⚠ |
| Pillow lava mound | A pile of rounded bulbous lobes with glassy cracked crusts, stacked where lava met water | 2.2 | Ocean | `built` |
| Black smoker chimney | A narrow ragged spire built up from the sea floor, flaring and lumpy, black to rust | 3.0 | Ocean | `built` ⚠ |
| Hydrothermal mound | A low mound of crumbling sulphide around a vent field, encrusted and rust-coloured | 2.4 | Ocean | `built` |
| Submerged limestone pavement | Flat blocks separated by deep straight water-widened joints — a drowned karren floor | 3.0 | Ocean, Beach | `built` |
| Lava-tube bench | A flat shelf of basalt with a collapsed edge and one straight overhung lip | 3.4 | Ocean, Beach | `built` |
| Storm-cast boulder | A very large angular block thrown clear above the tideline and left sitting on flat rock | 2.4 | Beach, Bare rock | `built` |
| Beachrock slab | A cemented sand slab dipping seaward in sheets | 2.2 | Beach | `shipped: beachrock-slab` |
| Boulder beach | Rounded storm-piled cobbles | 1.2 | Beach | `shipped: boulder-beach` |
| Blowhole | A sea-driven vent through a headland | 4.0 | Beach | `shipped: blowhole` |
| Rockpool platform | A flat wave-cut bench with shallow pools and a seaward step | 2.6 | Beach | `shipped: rockpool-platform` |
| Tidal notch | A cliff foot with a horizontal groove cut at one height | 2.4 | Beach | `shipped: tidal-notch` |
| Wave-polished boulder | Rounder and smoother than a river cobble, no facets left | 1.3 | Beach | `shipped: wave-polished-boulder` |
| Sea arch / wave-cut stack | Coastal erosion set pieces | 3–9 | Beach | `shipped: sea-arch`, `wave-cut-stack` |
| Sand ripple field | Regular parallel sand waves over a flat bottom | — | Ocean | `blocked: distribution`, §8.3 |

⚠ **Maerl bed** is a bed, not a nodule. `rock.clasts` has a **0.08 m floor**, so
the individual knuckles are drawn at 8 cm against a real 2–5 cm (*estimate*) —
about twice life size, in the same trap `conglomerate-boulder` records.

⚠ **Honeycomb worm reef.** The tubes are 5 mm across (*estimate*). At 10 cm they
are a twentieth of a voxel; `rock.cavernous` gives a pitted surface at decimetre
scale and that is what ships. The honeycomb is a texture ask.

⚠ **Black smoker.** `rock.flatten` at 3.6 with three lumps builds the spire, but
the generator only cuts and a chimney is *accreted* — the same limitation
`tufa-curtain` records for itself. The flare at the base comes from lump spread,
not from growth.

### 4.9 Intertidal and splash-zone plants (Beach biome)

Nineteen splash-zone and dune species already ship — `glasswort`,
`sea-purslane`, `sea-sandwort`, `sea-couch`, `marram-grass`, `lyme-grass`,
`sea-rocket`, `sea-kale`, `sea-holly`, `sea-lavender`, `coastal-thrift`,
`sea-aster`, `sea-bindweed`, `yellow-horned-poppy`, `beach-morning-glory`,
`smooth-cordgrass`, `bayberry`, `beach-rose`, `sea-buckthorn`. What is missing
is the **rock** end of the shore rather than the sand end:

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Rock samphire | Fleshy blue-green forked succulent leaves in a low bushy clump with flat yellow flower heads | 0.35 | Beach | `flower` | 5 cm | `built` |
| Golden samphire | Narrow fleshy leaves and bright yellow daisy heads, on saltmarsh edges and cliffs | 0.4 | Beach | `flower` | 5 cm | `built` |
| Sea campion | A low grey-green cushion studded with white flowers over inflated papery calyces | 0.25 | Beach, Bare rock | `flower` | 5 cm | `built` |
| Sea beet | Sprawling glossy dark-green leathery leaves with a long dull flower spike | 0.5 | Beach | `grass` | 5 cm | `built` |
| Sea-blite | A stiff succulent shrublet of narrow blue-green cylindrical leaves, reddening in autumn | 0.35 | Beach | `grass` | 5 cm | `built` |
| Sea rush | Stiff sharp-pointed cylindrical dark-green stems in a dense saltmarsh tussock | 0.8 | Beach | `reed` | 5 cm | `built` |
| Common saltmarsh-grass | A short dense grey-green turf covering the whole middle marsh | 0.3 | Beach | `grass` | 5 cm | `built` |

---

## 5. Freshwater

**No ocean weights anywhere in this section.** Fresh water is inside the land
biomes; see §1.

### 5.1 Submerged plants

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Curled pondweed | Translucent olive strap leaves with strongly wavy crisped edges, in loose alternating ranks | 0.6 | Temp. forest, grassland, taiga | `grass` | 5 cm | `built` |
| Canadian waterweed | Dense whorls of three small blunt leaves packed tight up a brittle stem | 0.4 | Temp. forest, grassland, taiga | `grass` | 5 cm | `built` |
| Rigid hornwort | Stiff brittle dark whorls of forked bristle leaves; rootless, hanging in mid-water | 0.6 | Temp. forest, grassland, rainforest | `grass` | 5 cm | `built` |
| Spiked water-milfoil | Feathery whorls of four finely divided leaves up a long limp stem, with a spike held clear of the surface | 1.0 | Grassland, temp. forest, taiga | `grass` | 5 cm | `built` |
| River water-crowfoot | Long trailing streamers of thread leaves combed downstream, with white five-petalled flowers on the surface | 1.4 | Temp. forest, grassland | `flower` | 5 cm | `built` |
| Common stonewort | Brittle grey-green calcified whorls, rough to the touch, smelling of garlic; no true roots | 0.35 | Grassland, temp. forest, tundra | `grass` | 5 cm | `built` |
| Ribbon-weed | Very long flat green ribbons rising straight from the bed and spiralling at the surface | 0.8 | Rainforest, savanna, grassland | `grass` | 5 cm | `built` |
| Common water-starwort | A small pale rosette of spoon leaves floating flat, with a submerged tail below | 0.25 | Temp. forest, grassland, taiga | `grass` | 5 cm | `built` ⚠ |
| Fanwort | Opposite pairs of fan-shaped finely divided leaves, flat as a hand, all in one plane | 0.5 | Rainforest, savanna | `grass` | 5 cm | `built` |
| Quillwort | A rosette of stiff dark cylindrical quills off a corm, in cold clear stony lakes | 0.22 | Tundra/alpine, taiga | `grass` | 5 cm | `built` ⚠ |
| Water soldier | An aloe-like rosette of stiff saw-edged spiny leaves that rises to the surface and sinks again | 0.4 | Temp. forest, grassland | `grass` | 5 cm | `built` |
| Needle spike-rush | A fine bright-green lawn of hair-thin quills over the shallow bed | 0.22 | Grassland, temp. forest | `grass` | 5 cm | `built` ⚠ |

⚠ **Water-starwort, quillwort, needle spike-rush** are authored at 0.22–0.25 m
against real 0.05–0.20 m (*estimates*) for the 20 cm floor. Each says so in its
own `notes`.

### 5.2 Floating-leaved plants

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| White water-lily | Big round notched pads lying flat on the surface with a many-petalled white cup among them | 0.35 | Temp. forest, grassland | `flower` | 5 cm | `built` ⚠ |
| Yellow water-lily | Larger leathery oval pads and a small globular yellow flower held clear on a thick stalk | 0.4 | Temp. forest, grassland, taiga | `flower` | 5 cm | `built` |
| Sacred lotus | Round blue-green pads held **above** the water on stiff stalks, with a huge pink bowl flower higher still | 1.2 | Rainforest, savanna | `flower` | 5 cm | `built` |
| Giant water-lily | Pads two metres across with a vertical upturned rim all the way round, like a floating tray | 0.6 | Rainforest | `flower` | 5 cm | `built` ⚠ |
| Fringed water-lily | Small heart-shaped pads and a bright yellow flower with deeply fringed petals | 0.25 | Temp. forest, grassland | `flower` | 5 cm | `built` |
| Water hyacinth | A rosette of glossy round leaves on swollen bulb-like inflated stalks, with a violet spike | 0.5 | Rainforest, savanna | `flower` | 5 cm | `built` |
| Water lettuce | A floating rosette of thick fluted velvety pale-green leaves, like an open cabbage | 0.25 | Rainforest, savanna | `grass` | 5 cm | `built` |
| Water chestnut | A flat rhombic rosette of toothed leaves on inflated stalks, spread like a snowflake | 0.25 | Temp. forest, grassland | `grass` | 5 cm | `built` |
| Duckweed | Single flat green discs 3–5 mm across covering the whole surface | — | — | — | — | `blocked: below floor`, §8.5 |
| Frogbit | Small kidney-shaped floating pads with a three-petalled white flower | 0.15 | Temp. forest | — | — | `blocked: below floor` |

⚠ **White water-lily.** The pad is the species and the tuft generator has no pad
— `tuft.head` draws a bloom, a spike or a plume on top of a stem, and a lily pad
is a flat horizontal disc lying ON a surface that does not exist in the asset.
What ships is a bloom-headed tuft at pad diameter, which reads as a lily from
above and as a bouquet from the side. Recorded in the spec.

⚠ **Giant water-lily.** Real pads are 2–3 m across (*estimate*) and
`tuft.head_m` caps at **8.0 m**, so the diameter is expressible — but the
upturned rim, which is the entire species, is a 5–10 cm vertical lip on a disc
and there is no rim in the generator at any lattice. Authored as a large plain
pad.

### 5.3 Emergent plants not already authored

Already shipped: `bulrush`, `water-reed`, `reed-sweet-grass`, `giant-reed`,
`papyrus`, `floodplain-sedge`, `smooth-cordgrass`, `marsh-marigold`,
`common-cottongrass`, `tussock-cottongrass`, `wood-horsetail`. Reconciled
against the terrestrial pass before authoring; none of the below duplicates one.

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Broadleaf cattail | Flat grey-green straps and a fat brown cigar seed head on a bare stalk above them | 2.2 | Grassland, temp. forest, taiga | `reed` | 5 cm | `built` |
| Branched bur-reed | Zig-zag stems with spiky spherical burr heads scattered along them | 1.0 | Temp. forest, grassland, taiga | `reed` | 5 cm | `built` |
| Sweet flag | Sword leaves with a crimped wavy edge and an odd finger-like flower spike off the side of the stem | 1.0 | Temp. forest, grassland | `reed` | 5 cm | `built` |
| Water horsetail | Bare jointed hollow grey-green tubes standing dead straight with almost no branching | 0.9 | Taiga, temp. forest, tundra | `reed` | 5 cm | `built` |
| Wild rice | A very tall coarse grass with a wide open flowering panicle at the top | 2.0 | Grassland, temp. forest | `reed` | 5 cm | `built` |
| Soft rush | A dense tussock of smooth glossy cylindrical stems with the flower cluster bursting out of the side | 0.9 | Grassland, temp. forest, taiga | `reed` | 5 cm | `built` |
| Lesser pond-sedge | Broad drooping keeled leaves and long nodding brown-black flower spikes | 1.0 | Temp. forest, grassland | `grass` | 5 cm | `built` |
| Yellow flag iris | Stiff grey-green sword leaves in a flat fan with a large bright yellow flag flower | 1.0 | Temp. forest, grassland, taiga | `flower` | 5 cm | `built` |
| Flowering rush | Long three-cornered rush leaves and one tall stalk carrying an umbel of pink flowers | 1.2 | Grassland, temp. forest | `flower` | 5 cm | `built` |
| Arrowhead | Arrow-shaped leaves held vertically clear of the water on long stalks, with white three-petalled flowers | 0.8 | Temp. forest, grassland | `flower` | 5 cm | `built` |
| Pickerelweed | Glossy heart-shaped leaves and a dense blue-violet flower spike above them | 0.8 | Grassland, savanna | `flower` | 5 cm | `built` |
| Common water-plantain | A basal rosette of oval ribbed leaves with a tall airy pyramid of tiny pale flowers | 0.8 | Grassland, temp. forest | `flower` | 5 cm | `built` |
| Purple loosestrife | A tall stiff spire of magenta flowers over a whorled leafy stem, in dense stands | 1.2 | Grassland, temp. forest | `flower` | 5 cm | `built` |
| Water mint | A low sprawling clump with rounded lilac flower heads and a strong smell | 0.5 | Temp. forest, grassland | `flower` | 5 cm | `built` |
| Water forget-me-not | Trailing low stems with sprays of small sky-blue flowers with a yellow eye | 0.3 | Temp. forest, taiga, grassland | `flower` | 5 cm | `built` |
| Bogbean | Three thick oval leaflets held clear of the water and a spike of white starry fringed flowers | 0.3 | Taiga, tundra/alpine | `flower` | 5 cm | `built` |
| Marsh cinquefoil | Sprawling with dark purple-red five-petalled flowers and toothed pinnate leaves | 0.4 | Taiga, temp. forest, tundra | `flower` | 5 cm | `built` |
| Watercress | Low dense dark-green pinnate mats spreading over gravel in a spring-fed stream | 0.3 | Temp. forest, grassland | `grass` | 5 cm | `built` |

### 5.4 River mosses and liverworts

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Willow moss (Fontinalis) | Long dark-olive trailing streamers of keeled leaves, anchored to a rock and combed downstream | 0.35 | Temp. forest, taiga, tundra | `grass` | 5 cm | `built` |
| Brook moss cushion | A dense low bright-green cushion on a splashed boulder at the water's edge | 0.22 | Taiga, temp. forest, tundra | `grass` | 5 cm | `built` ⚠ |
| Water earwort | Dark reddish overlapping scale leaves in a flat mat on acid-stream stones | 0.2 | Tundra/alpine, taiga | `grass` | 5 cm | `built` ⚠ |
| Overhanging liverwort curtain | A thin dark film over a permanently wet rock face | — | — | — | — | `blocked: material`, §8.5 |

⚠ Both are authored at 0.20–0.22 m against real 0.03–0.10 m (*estimates*) —
2–7×, the largest authored-up in this document, and the honest alternative was
to leave the wet-boulder layer empty. Written into both specs' `notes`.

### 5.5 Algal mats

| Species | Voxel-artist description | Size (m) | Biomes | Kind | Lattice | Status |
|---|---|---|---|---|---|---|
| Blanket weed | Coarse bright-green cotton-wool filaments streaming from a stone, slimy in the hand | 0.4 | Grassland, temp. forest | `grass` | 5 cm | `built` |
| Diatom biofilm | The brown slippery film on every submerged stone | — | — | — | — | `blocked: material`, §8.5 |
| Cyanobacterial mat | A dark blue-green scum layer over warm shallow mud | — | — | — | — | `blocked: material`, §8.5 |

### 5.6 Freshwater rocks — all `rock`, all 10 cm

| Species | Voxel-artist description | Size (m) | Biomes | Status |
|---|---|---|---|---|
| River cobble | Rounded waterworn stone, low angularity, heavy erosion | 0.9 | 5 land biomes | `shipped: river-cobble` |
| Gravel bar | A flat elongated lens of loose rounded material on the inside of a bend | 2.4 | 4 land biomes | `shipped: gravel-bar` |
| Quartz-vein cobble bar | As gravel bar, with a white vein through it | 1.5 | — | `shipped: quartz-vein-cobble-bar` |
| Rapids whaleback | A smooth grey dome in the channel, undercut at low water | 2.4 | 4 land biomes | `shipped: rapids-whaleback` |
| Streambed cascade block | Angular blocks wedged in a channel, tops water-polished | 1.2 | 5 land biomes | `shipped: streambed-cascade-block` |
| Tufa curtain | Lobed drapery of spring limestone, built up rather than cut down | 2.0 | — | `shipped: tufa-curtain` |
| Plunge-pool boulder | A very large rounded block sitting in the scoured basin below a fall, half drowned | 2.2 | Temp. forest, taiga, tundra, rainforest | `built` |
| Waterfall lip ledge | A hard flat capping bed with a clean straight edge and a deeply undercut soft bed beneath | 3.0 | Temp. forest, taiga, tundra, rainforest | `built` |
| Bedrock pothole | A block with a deep smooth cylindrical hole drilled through it by a trapped grinding stone | 1.6 | Temp. forest, taiga, tundra | `built` ⚠ |
| Riffle slab | A low flat-topped bedrock plate breaking the surface in shallow fast water | 1.2 | Temp. forest, grassland, taiga | `built` |
| Step-pool boulder | A wedged angular block spanning a steep mountain stream, forming one step of the staircase | 1.5 | Tundra/alpine, taiga, bare rock | `built` |
| Lake-bed slab | A broad flat plate lying almost level under still water, silt-edged, barely proud | 2.0 | Temp. forest, grassland, taiga, tundra | `built` |
| Undercut bank block | A block in the bank with the water having cut a horizontal slot beneath it at one height | 2.0 | Temp. forest, grassland, rainforest | `built` |
| Travertine rimstone dam | A curved lip of pale banded limestone holding back a shallow terrace pool | 2.4 | Grassland, temp. forest, savanna | `built` ⚠ |
| Tufa spring mound | A lumpy pale mound built up around a hard-water spring, riddled with holes | 1.8 | Grassland, temp. forest, savanna | `built` ⚠ |
| Marl bench | A soft pale chalky shelf of lake-bed carbonate, crumbling at the edge | 1.8 | Grassland, temp. forest | `built` |

⚠ **Bedrock pothole.** `rock.pans` hollows a **top surface** where water has
nowhere lower to go; it does not bore a shaft. What ships is a deep dished
basin, not a through-hole. §8.4.

⚠ **Travertine dam and tufa mound** are both accretionary and the generator only
cuts, which is the limitation `tufa-curtain` already records for itself. The
banded lip comes from `rock.bedding` at a thin bed thickness, which is the right
*look* arrived at by the wrong *mechanism*, and both specs say so.

---

## 6. Adopted and rejected

The house standard for this section is `docs/fish-shape-research.md` §8 and
`docs/bird-shape-research.md`: **a rejection carries the number that decided it,
not an opinion.** The sizes in §4 and §5 follow the *other* house convention,
the one the ten biome files set — approximate figures from general knowledge,
labelled once and not dressed with decimals. §9 says why the document mixes the
two.

### Adopted

* **Coral splits by branch thickness, at about 10 cm.** Massive forms are
  `rock` and terrain-lattice; branching forms are `bush` at 2 cm. Measured: the
  same coral is 5,302 voxels at 2 cm and 397 at 10 cm, and its branch tip is
  0.50 voxels at 10 cm. §3.1.
* **Kelp stays at 5 cm and at 28 m authored.** Measured: 18,341 voxels, 1.3 s —
  1.7% of one `temperate-oak`. §3.2.
* **Anemones, sea pens and tube worms are authored with the plant generators**
  and are labelled as animals in their own `notes`. A snakelocks anemone is a
  mop of long soft filaments from a common base, which is what `tuft` is; the
  alternative was a new generator for a shape that already exists.
* **Freshwater plants carry land-biome weights and no ocean weight.** Follows
  the shipped freshwater fish convention exactly (`specs/brown-trout.json`).
* **Depth goes in `notes`, in words.** §3.3.
* **One organism per spec.** A kelp bed, a seagrass meadow, a coral reef and a
  cobble bed are each *many* organisms; what is authored is one frond, one tuft,
  one coral head, one cobble, and placement makes the bed. This is not a style
  choice — `tools/buildcheck.py` enforces `pieces == 1` at 26-connectivity.

### Rejected, with the number

* **A `coral` generator.** Rejected: the two shapes a reef needs both already
  exist. The dome forms measured identical to a rock in `brain-coral`'s own
  shipped notes ("its DOME needed no retuning at all"), and the branching forms
  build one connected piece at every lattice from 1 cm to 10 cm (table, §3.1).
  What a coral generator would add over `bush` is finer branch spacing than
  `growth.kill_m`'s 0.10 m floor — one floor, in one existing file, not a
  generator.
* **A `kelp` generator**, asked for by `00-ocean.md` for the surface canopy.
  Rejected on cost of the alternative: the missing feature is along-the-stem
  foliage, which is one drawing pass on the existing tuft, and the 18,341-voxel
  measurement removes the performance argument that motivated a separate one.
* **Authoring kelp at 10 cm to save memory.** Rejected: it removes 84% of the
  plant's voxels (18,341 → 3,189) and takes the stipe from 2 voxels through to
  the 1-voxel minimum, to save 46 MB of transient grid against a 3 GB ceiling.
* **Duckweed, diatom film, cyanobacterial mat, encrusting sponge, coralline
  crust, liverwort curtain.** Rejected as geometry: all are 0.5–5 mm thick. The
  coarsest lattice available is 10 cm for a rock and the finest anywhere is
  1 cm, so they miss the three-voxel rule by one to two orders of magnitude, and
  everything identifying about them is colour and patch outline. This is
  `00-ocean.md`'s verdict on coralline crust and `biomes/README.md` §3's verdict
  on lichen, and it holds for all six. **They belong in the material palette.**
* **Mushroom coral.** Rejected on a size-against-`flatten` sweep: it needs
  1.10 m — four times life size — to reach the voxel count of a 0.9 m river
  cobble. §8.7.
* **The `bush` kind below about half a metre.** Rejected on five measured
  species: 45–168 voxels against a shipped small-tuft band of 55–173, and one
  outright health failure. §8.8.
* **Beadlet anemone (0.05 m), mermaid's wineglass (0.05 m), maerl nodule
  (0.06 m), frogbit (0.15 m).** Rejected against the 20 cm floor: authoring up
  would be 4×, 4×, 3.3× and 1.3×. The library's shipped precedent is
  `clown-anemonefish` at **2.2×**, and the largest thing this pass was willing
  to author up is 2–7× for the two river mosses, which is written into their
  specs and argued for on the grounds that the alternative is an empty layer.
  For the first three the alternative is not empty — the maerl *bed* and the
  anemone-bearing rock both exist as other rows.
* **A `sponge` kind.** Rejected: measured against the same test as coral. Four
  of the five sponges here are a lump, a barrel, a fan on edge or a cup, all of
  which `rock` builds at 10 cm with a metre or more of relief; only the
  through-hole is missing, and that is a subtraction feature (§8.4) rather than
  a kind.
* **`rock.clasts` for barnacle crust and honeycomb-worm tubes.** Rejected on the
  parameter's own floor: `clast_size_m` bottoms at **0.08 m** against real
  barnacles at 1–3 cm and worm tubes at 5 mm (*estimates*). At 10 cm a clast is
  one voxel of relief and reads as noise, which is the conclusion
  `conglomerate-boulder` already recorded for pebbles.

---

## 7. What was built, and where the seed scripts are

| Script | Kinds | Specs |
|---|---|---|
| `tools/seed_saltwater_plants.py` | `reed` 12, `grass` 19, `bush` 8, `flower` 9 | **48** |
| `tools/seed_saltwater_rocks.py` | `rock` | **23** |
| `tools/seed_freshwater_plants.py` | `grass` 17, `reed` 6, `flower` 19 | **42** |
| `tools/seed_freshwater_rocks.py` | `rock` | **10** |
| | | **123** |

Every script refuses to overwrite an existing spec (`tools/seedspec.py`), so
re-running one is safe and `--force` is the loud way to revert.

**Cost, measured.** All 123 at seed 1 come to **88,300 voxels** between them —
**8% of one `temperate-oak`** at 1,065,343. The largest single asset in the pass
is `giant-water-lily` at 19,594–26,139 (a 1.6 m pad is 32 voxels across); the
smallest is `sea-pen` at 37–52, against a shipped `glasswort` at 55. Nothing
here is expensive and nothing here is below shipped practice.

### Verification

    python -m forge.cli selftest          # PASS, 824 specs
    python tools/buildcheck.py            # PASS
    python tools/buildcheck.py --seeds 1 2 3 <the 123>   # PASS, 369 builds
    python tools/aquaticprobe.py --tuft   # 46 mechanisms, 0 dead, 0 faint

`tools/aquaticprobe.py` is new and is the reason to trust the rock work. This
pass leans on eleven rock mechanisms — columns, bedding, pans, clasts, notch,
rind, flutes, caprock, aspect, joint sets, block relief — at sizes none of them
was tuned for, and `brain-coral` already records one of them (`rock.flutes`)
being turned up from a setting that moved the silhouette **5.8%** and was
invisible in a render. The probe builds each spec twice at the same seed, once
with the mechanism at its authored value and once with it off, and reports the
voxel move and the silhouette move. **All 46 checks pass; the weakest is
`bubble-coral`'s lump count at 12.0% of silhouette and the strongest is
`sea-palm`'s arc at 85.0%.** Renders are in `out/aquatic/`.

---

## 8. What could not be built, and the number that stopped it

### 8.1 Sea fan / gorgonian — a placement fault, not a shape fault

Recorded by the pass that opened the sea floor and reproduced here rather than
re-litigated. A gorgonian is a flat rigid **net** held across the current: a
plane, not a volume, with 1–3 cm mesh openings (*estimate*). The only kind in
the library that produces a plane is `fish`, whose fin plates measure a genuine
0.98 × 0.06 × 2.63 m sail — but a `fish` spec is placed as a *swimming* detail
entity, so a gorgonian authored that way swims in mid-water. The `bush` route
measures 1.04 × 1.20 × 1.04, because `crown.squash` acts vertically and nothing
flattens a crown horizontally, and the tuft kinds draw round capsules over a
full circle of azimuths.

**What would fix it is a placement flag** — "this detail entity is anchored to
the bottom and oriented" — not a new generator.

### 8.2 `flower` is not in the ocean `hosts` tuple

`biomes.BY_KEY['ocean'].hosts` is `('fish', 'cetacean', 'bird', 'rock', 'grass',
'reed', 'bush')`. **`flower` is absent.** That is defensible for real flowering
plants, since the only ones in the sea are the seagrasses and those are `grass`
here. It bites on exactly one row: **plumose anemone**, whose frilly white crown
on a bare column is a `flower` shape and nothing else, and which is therefore
authored Beach-only. One word in a file I do not own; recorded rather than
requested, because the species has a real Beach range and is not lost.

**Bare rock is the same problem twice more, and both were mine.**
`biomes.BY_KEY['bare_rock'].hosts` is `('rock', 'bird', 'quadruped')` with
`plantable` False — **no plant kind at all**. Rock samphire and sea campion both
genuinely grow on cliff faces steeper than the 70% grade at which the engine
classifies ground as bare rock, and both were first authored with a bare-rock
weight. **Both were accepted with no warning**, because `spec.py:488` passes
`kinds=b.hosts` to the biome-weight parameter and `kinds` gates *which sliders
the app shows*, not what validation allows. A species can therefore be authored
into a biome that will never place it, and nothing anywhere says so.

`python tools/aquaticprobe.py --hosts` is the check, and it sweeps the whole
library rather than this pass. Both were removed and each spec's `notes` records
what was given up. The probe currently reports **two remaining violations, both
predating this pass and neither mine to change**: `herb-robert` (a `flower`
weighted 0.20 to bare rock) and `moss-cushion` (a `grass` weighted 0.25). They
belong to the wildflower and ground-cover passes.

### 8.3 Distributions are still not assets

`sand ripple field`, `mussel bed`, `barnacle crust`, `cobble bed` and `kelp
forest` are all *fields* of a repeated unit rather than one unit.
`tools/buildcheck.py` enforces one connected piece, and `biomes/README.md` §3
already books this against a placement feature that serves blockfield, talus
cone, patterned ground, erratic train, rock glacier and shingle bank. Six rows
there, five more here, one feature.

Each of those five is nevertheless represented by its **unit**: one cobble, one
maerl bed lens, one coral-rubble heap, one kelp plant.

### 8.4 The rock generator only cuts, and three shapes want a hole

`rock.pans` hollows a top surface; there is nothing that bores through.

* **Barrel sponge** — a metre-deep central well through most of the body.
* **Bedrock pothole** — a smooth cylindrical shaft.
* **Black smoker chimney** — a vertical conduit up the middle.

All three ship as their solid equivalent with a dished top, which reads at
distance and is wrong in the hand. `rock.arch` is the only through-cut in the
generator and it is a horizontal span, not a vertical bore.

The same limit from the other direction: **the generator has no accretion**, so
the travertine dam, the tufa mound and the black smoker — all three of which are
*built up* rather than eroded down — are carved approximations. `tufa-curtain`
already records this about itself and it is unchanged.

### 8.5 Six species are a material, not geometry

Listed in §6. 0.5–5 mm thick against a 1 cm finest lattice.

### 8.6a `materials.head` and `materials.stem` are two different menus, and the mismatch is silent

> **CLOSED 2026-08-15.** They are one menu now — `spec._PLANT_MATERIALS`, read
> by both rows, with `forge/ground.py` asserting at import that they still
> share it, and `selftest` tripping a bogus choice through five menus every run
> to prove the alarm still fires. `podzol` is the dark brown this section says
> `head` has none of, and `skin_red` was added for the dark red. No spec was
> re-coloured. See `docs/trunk-taper-and-the-deferred-five.md` §5.

Found by writing five specs wrong. `materials.stem` offers seven land-vegetation
greens and browns; `materials.head` offers fourteen and **they do not overlap** —
there is no `leaf_jungle` and no `podzol` in `head`. A head material outside its
menu is **not refused, it is replaced with `leaf_blossom`**, a pink at
(226,168,190).

So the first draft of `tools/seed_freshwater_plants.py` shipped a brown-cigar
cattail, a black-spiked pond sedge, and two sets of lotus pads, **all wearing
blossom pink**, and every one of them validated clean and built clean. That is
this project's signature failure exactly. The warning *is* printed by the seed
script; it is worth reading rather than scrolling past.

Related and unfixed: **there is no dark brown in `materials.head` at all.**
`leaf_autumn` (192,122,46) is the darkest warm entry and is an orange, so
`broadleaf-cattail` and `lesser-pond-sedge` both carry it and both say so. And
there is no dark red, which costs `marsh-cinquefoil` its entire identity — its
blackish purple-red petals ship as `plume_crimson` (208,40,56), a bright orange
red.

### 8.7 A mushroom coral cannot be a rock at 10 cm

The only species this pass authored and then withdrew. A free-living *Fungia* is
a disc 0.10–0.25 m across (*estimate*) and a few centimetres thick; at the 10 cm
a `rock` is **locked** to, that is 2.5 voxels across and under one voxel thick.
**The whole organism fails the three-voxel rule**, before anyone asks about the
radial septa that name it.

Swept, three seeds each:

| Authored size | `flatten` | Voxels |
|---|---|---|
| 0.75 m | 0.42 | 21–25 |
| 0.75 m | 0.55 | 40–52 |
| 0.90 m | 0.55 | 49–60 |
| 0.90 m | 0.70 | 57–82 |
| 1.10 m | 0.55 | **82–113** |

It takes **1.10 m** to build an asset with as many voxels as a 0.9 m
`river-cobble` (142), and 1.10 m is **four times life size** against a shipped
authoring-up ceiling of 2.2× (`clown-anemonefish`). Broadening the row to the
large free-living fungiids — *Herpolitha*, *Ctenactis*, which reach about half a
metre — still leaves it at 2× with 60 voxels.

**There is no fix on this side**, because the lattice is not free for a `rock`.
A mushroom coral needs a detail-lattice kind, and every detail-lattice kind here
is a branching skeleton or a spray of stems, and a fungiid is a solid disc.

### 8.8 The bush generator has a floor at about half a metre

> **CLOSED 2026-08-15, and the floor turned out to be the LATTICE.** Eight
> growth steps of 8 cm is 0.64 m, which is why `sea-oak-weed` at 0.60 m is the
> smallest bush that works — to within a voxel of the arithmetic. The four
> floors are lowered to what the finest legal lattice can carry, and the same
> 0.25 m plant then builds 171–196 voxels with fork order 3–5 **at 2 cm**,
> against 66–89 at 5 cm with the old floors. The five species were left as
> tufts. See `docs/trunk-taper-and-the-deferred-five.md` §4.

**Five species moved from `bush` to a tuft kind because of it**, and the numbers
are worth recording once because nothing else in the repo states them together.
Four `bush` parameters have hard floors:

    crown.radius_m       0.30 m
    growth.influence_m   0.40 m
    growth.step_m        0.08 m
    trunk.radius_base_m  0.05 m

On a 25 cm plant **the crown radius floor is 1.2× the whole plant's height, the
influence radius is 1.6× it, and one growth step is a third of it.** Every
growth target is outside the plant. Measured, as bushes:

| Species | Voxels, seeds 1–5 |
|---|---|
| `red-comb-weed` | 45, 133, 53, 162, 123 |
| `irish-moss` | 102 |
| `green-sea-fingers` | 119 |
| `halimeda` | 153 |
| `sea-oak-weed` (0.60 m) | 168 |

**`dead-mans-fingers` failed outright**, not merely thinly: `pipeline.health`
rejected seed 2 with *"bare: the trunk never branched"*, having built 1,079
voxels at seed 1. A species that is fine on one individual and structurally
broken on the next is the worst version of this.

Re-authored as tufts, the same four build 94–185 voxels — which is exactly the
shipped band for a small tuft (`glasswort` 55, `sea-sandwort` 100, `feather-moss`
128, `moss-cushion` 163, `wild-thyme` 173). **`sea-oak-weed` at 0.60 m is the
smallest bush in the library that still works**, and it needs three of the four
floors *at* the floor plus a growth point count of 1400 to do it.

What is lost by the move is **forking** — the tuft generator has no branching at
all — which for a dichotomously forking red alga is real. What is gained is an
asset instead of a scribble.

### 8.6 `placement.elev_min_m` cannot reach the sea floor

Floor of **−10.0 m**, measured (§3.3). A kelp forest at 20–40 m, a Lophelia reef
at 200–1,000 m and a black smoker at 2,000 m all author `-10.0` and every ocean
spec in the library is pinned there. This is a real range gate that would matter
the moment a scatterer reads it, and every deep species below is currently
indistinguishable from a shallow one.

---

## 9. Where the numbers come from

**This document deliberately mixes two house conventions, and says which is
which at every point of use.**

**Measured, on this machine, reproducible:**

* Every voxel count, build time, grid size and branch-tip figure in §3.1 and
  §3.2. Built with `pipeline.build(spec, 1, resolution_cm=…)` on the specs
  named.
* The `placement.elev_min_m` clamp and the `detail.depth_min_m` silent accept in
  §3.3, both reproduced with `spec.patch` and the warning text quoted verbatim.
* Every parameter floor and ceiling quoted (`growth.kill_m` 0.10,
  `rock.clast_size_m` 0.08, `tuft.head_m` 8.0, `placement.elev_min_m` −10.0),
  read out of `spec.PARAMS`.
* Every `hosts` tuple, spec name, authored size and line reference.

**Estimated, from general knowledge, and marked `(estimate)` at the point of
use:**

* **Every species size in §4 and §5.** These are approximate typical adult
  figures. None is measured and none is sourced. They are good enough to choose
  a voxel lattice, which is what they are for, and **not** good enough to quote
  as fact.
* Every feature dimension used in an authored-up or a rejection: staghorn branch
  2–5 cm, brain-coral groove 5–10 mm, kelp bladder 2–5 cm, gorgonian mesh
  1–3 cm, barnacle 1–3 cm, worm tube 5 mm, duckweed frond 3–5 mm, lily rim
  5–10 cm. The *conclusions* drawn from them are arithmetic and are sound at any
  plausible value in the ranges given; the ranges themselves are recall.

**Nothing here is cited, and that is deliberate.** This project has already
shipped a fabricated citation — an orca eye patch documented as "measured" at
figures that turned out to be dimensionless indices from an unrelated paper —
and a second agent found the identical trap in a different source. This
session's search budget is exhausted, so the honest position is a blanket
approximation stated once and marked at every load-bearing use, rather than a
sprinkling of plausible-looking decimals with no way to check them. That is the
convention `docs/biomes/README.md` §8 set for exactly this reason.

Where a number *decides* something — every entry in §6's rejection list — it is
either measured, or it is arithmetic whose answer does not change anywhere in
the estimated range. Those are the only two kinds of number this document acts
on.
