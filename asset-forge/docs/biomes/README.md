# Master species lists, by biome

**816 distinct species across 1,049 biome entries**, covering trees, rock types,
flowers, ground cover, bushes, birds, land animals and fish for all ten of the
world's biomes. These are the **build queue for asset-forge**: one file per
biome, every entry carrying a description a voxel artist can work from, an
approximate real size in metres, and a status saying whether it ships today,
needs authoring, or needs a generator that does not exist.

| file | biome | share of land | entries |
|---|---|---|---|
| [`00-ocean.md`](00-ocean.md) | Ocean | — (not land) | 116 |
| [`01-beach.md`](01-beach.md) | Beach | 5.54% | 125 |
| [`02-grassland.md`](02-grassland.md) | Grassland | **28.06%** | 211 |
| [`03-temperate-forest.md`](03-temperate-forest.md) | Temperate forest | 4.90% | 197 |
| [`04-rainforest.md`](04-rainforest.md) | Rainforest | 4.73% | 74 |
| [`05-desert.md`](05-desert.md) | Desert | 9.74% | 70 |
| [`06-savanna.md`](06-savanna.md) | Savanna | **20.76%** | 75 |
| [`07-taiga.md`](07-taiga.md) | Taiga | 6.64% | 71 |
| [`08-tundra-alpine.md`](08-tundra-alpine.md) | Tundra / alpine | 19.62% | 65 |
| [`09-bare-rock.md`](09-bare-rock.md) | Bare rock | not measurable from the census | 45 |

---

## 1. The biome set, and where it is defined

**The world has exactly ten biomes**, and the definition lives in three places
that must agree bit-for-bit. All three were read for this document and **all
three agree** — same count, same ids, same order:

| # | Engine enum | Generator mirror | GPU mirror |
|---|---|---|---|
| 0 | `OCEAN` — `voxel-core/include/voxelcore/biome.h:24` | `biomes.py:57` | `voxel-core/shaders/worldgen.ush:140` |
| 1 | `BEACH` — `biome.h:25` | `biomes.py:59` | `worldgen.ush:141` |
| 2 | `GRASSLAND` — `biome.h:26` | `biomes.py:61` | `worldgen.ush:142` |
| 3 | `TEMPERATE_FOREST` — `biome.h:27` | `biomes.py:63` | `worldgen.ush:143` |
| 4 | `RAINFOREST` — `biome.h:28` | `biomes.py:65` | `worldgen.ush:144` |
| 5 | `DESERT` — `biome.h:29` | `biomes.py:67` | `worldgen.ush:145` |
| 6 | `SAVANNA` — `biome.h:30` | `biomes.py:69` | `worldgen.ush:146` |
| 7 | `TAIGA` — `biome.h:31` | `biomes.py:71` | `worldgen.ush:147` |
| 8 | `TUNDRA_ALPINE` — `biome.h:32` | `biomes.py:73` | `worldgen.ush:148` |
| 9 | `BARE_ROCK` — `biome.h:40` (appended at worldgen v8) | `biomes.py:79` | `worldgen.ush:149` |

`classifyBiome` is at `biome.h:209-235`; its bit-exact GPU mirror is at
`worldgen.ush:1144-1166`, under the do-not-drift note at `worldgen.ush:1132-1135`.
`biome.h:4-7` states the contract: any change must be
made identically in both and re-verified by `vxc_gpu`.

**There is no engine/generator disagreement about the biome set.** There are two
disagreements about what each biome may *host*, which is a different thing —
`hosts` is the generator's own concept and does not exist in the engine at all.
Both are recorded in §5.

### The climate envelopes

Species assignment in these files is defensible because it is keyed to the
envelope, not to the biome's name. **Three of the ten are decided by TERRAIN
before climate is ever consulted** (`biomes.py:8-14`), and that ordering matters
to a species list: a species tagged temperate forest will never appear on a cliff
face or above the treeline no matter how mild and wet the climate there is.

| Biome | What puts a column here | Source |
|---|---|---|
| **Ocean** | surface below −3 m | `biome.h:216`, threshold `:75` |
| **Beach** | surface −3 m to +4 m | `biome.h:217`, thresholds `:75-76` |
| **Bare rock** | slope steeper than a 70% grade (~35°, the angle of repose for soil and scree) | `biome.h:220`, threshold `:69-71`, reasoning `:52-68` |
| **Tundra / alpine** | above the treeline: 900 m at 0 °C, +150 m per °C (lapse rate 6.5 °C/km) | `biome.h:221`, constants `:98-99`. The 900 m base is **tuned, not derived**, and `:88-93` says so |
| **Taiga** | mean annual temperature below 5 °C, tested before any precipitation gate | `biome.h:225`, threshold `:120` |
| **Desert** | under 400 mm/yr **and** at least 24 °C | `biome.h:231`, thresholds `:134-135` |
| **Grassland** | arid and not hot; or 400–800 mm/yr without a marked dry season | `biome.h:231-232` |
| **Savanna** | 400–1600 mm/yr, ≥18 °C, **and** precipitation seasonality ≥70% CV — a dry season of about four months | `biome.h:232-233`, threshold `:184` |
| **Temperate forest** | 800–1600 mm/yr without the savanna conditions; or wetter than 1600 mm/yr and under 18 °C | `biome.h:233-234` |
| **Rainforest** | over 1600 mm/yr **and** at least 18 °C | `biome.h:234`, thresholds `:121`, `:137` |

Two of these constants carry history that a species list should not re-litigate:

* **The savanna gate reads precipitation seasonality (bio_15), not temperature
  seasonality (bio_4)**, and was rewritten at worldgen v22. `biome.h:139-184`
  records that the old gate was not mistuned but **unsatisfiable** — zero pixels
  of Earth land satisfied it — so savanna was dead code for fourteen worldgen
  versions. The 70% CV threshold is derived twice, physically and empirically,
  and both routes agree.
* **The 24 °C desert gate was re-examined at v22 and deliberately not moved**
  (`biome.h:122-133`). Lowering it to make more deserts appear would put half the
  world's dry temperate land under sand. This is why **grassland is a catch-all**
  that absorbs cold steppe, prairie, Mediterranean scrub and semi-arid shrubland
  under one id, and why its species list spans the widest climate range of any
  file here.

---

## 2. What is in the lists

| Category | Entries | Distinct species |
|---|---|---|
| Land animals | 215 | 172 |
| Birds | 187 | 130 |
| Fish | 173 | 134 |
| Rock types | 134 | 91 |
| Trees | 116 | 96 |
| Ground cover | 81 | 67 |
| Flowers | 75 | 70 |
| Bushes / shrubs | 68 | 56 |
| **Total** | **1,049** | **816** |

Entries exceed distinct species because a species appears in every biome it
belongs to — a red fox is in five files, a granite boulder in six — which is
exactly how the biome-weight system works (`biomes.py:98-107`).

By status:

| Status | Rows | Meaning |
|---|---|---|
| `queued` | 524 | A generator exists. Authoring work only. |
| `shipped: <spec>` | 282 | A spec exists in `asset-forge/specs/`. |
| `gen: <name>` | 223 | **Blocked on a generator that does not exist.** |
| `host: <kind>` | 15 | The biome's `hosts` tuple does not admit the kind yet. |

**All 111 specs currently in `asset-forge/specs/` are accounted for.** 109 are
marked `shipped:` on at least one row; the remaining two — `hero-arch-colossal`
and `hero-natural-arch` — are described in `05-desert.md`'s rock-section prose as
hero-scale versions of the tabled arch, rather than given rows of their own.

> The task brief said 107 specs. There are **111** on disk today: 34 rock, 20
> bird, 17 tree, 17 fish, 7 cetacean, 5 flower, 4 grass, 4 bush, 3 reed. The
> difference is not a discrepancy in these files — it is just that four specs
> landed after the brief was written.

---

## 3. The generator gaps

Six generators exist (`forge/kinds.py:66-135`): `tree`, `bush`, `rock`, the one
tuft generator behind `grass`/`reed`/`flower`, `fish` (with `cetacean` as the
same generator differently parameterised), and `bird`.

**Ten kinds of asset in these lists have no generator**, plus one thing that is
not a generator at all. Ordered by how many rows each one blocks:

| Gap | Rows blocked | What it is |
|---|---|---|
| **`quadruped`** | **167** | Four-legged terrestrial vertebrates. §4 is entirely about this one. |
| `arthropod` | 11 | Insects, spiders, scorpions, crabs. |
| `chelonian` | 9 | Turtles and tortoises — a shell plus a quadruped. |
| `serpentine` | 9 | Snakes and legless lizards. |
| `pinniped` | 7 | Seals, sea lions, walrus. |
| `rock` scatter | 5 | Not a generator — a **placement** feature. Blockfields, talus cones, patterned ground and rock glaciers are a *field* of blocks, not one block. One feature serves five entries in two files. |
| `coral` | 4 | Reef structure. |
| `cephalopod` | 3 | Octopus, squid, jellyfish — no rigid axis anywhere, which is precisely why no existing generator reaches them. |
| `succulent` | 3 | Only the prickly pear is a true gap. |
| `lichen` | 2 | A crust, not an object. |
| `fungus` | 1 | Mushrooms and bracket fungi. |

**Four of these are probably not new generators, and the files say why rather
than asserting a gap by reflex:**

* **`serpentine` is very likely the fish generator with the fins turned off.**
  The fish generator lofts a body whose cross-section varies along one axis, and
  `specs/river-eel.json` already ships at a `depth_ratio` of 0.085 — about 12:1 —
  against `docs/fish-shape-research.md`'s measured eel-class median of 16.8:1. A
  grass snake is roughly 15:1 and an adder roughly 17:1. **This is the cheapest
  test in the whole document and it should be run before the quadruped work is
  scoped**, because a cheap win changes the build order.
* **`succulent` is mostly the tree generator.** A saguaro and a Joshua tree are
  the whorl growth model with foliage off; a barrel cactus is a part-buried
  ribbed dome, which is the rock generator. Only the prickly pear genuinely
  resists, because a pad is a flat disc and every branch primitive in
  `forge/skeleton.py` is a cylinder.
* **`coral` splits in two.** A brain coral is a rock with a different palette; a
  branching stony coral is a self-similar branching structure, which is what
  `skeleton.py`'s `grow` already builds for trees. The shortest route to a reef
  is the tree generator with a stone palette and no leaves.
* **`lichen` is not geometry at any lattice this project has.** A crust is
  1–5 mm and the coarsest lattice available to a rock is locked at 10 cm by the
  terrain grid, so it misses the three-voxel rule by two orders of magnitude —
  and everything identifying about it is colour and patch outline. It belongs in
  the material palette.

---

## 4. What a quadruped generator needs

This is the single largest unlock in the library: **167 rows across all ten
biomes**, in the one category the world currently has none of. The owner's list —
kangaroo, deer, squirrel, wild boar, stag, fox, wild hare, gorilla, zebra,
antelope, alpaca, moose, bison, monkey, tiger, large lizards — is spread across
the files, and every one of them is blocked here.

There is a `bird` generator (jointed: body, neck, head, bill, tail fan, two
wings, two legs) and a `fish` generator (a lofted body with fin plates). **Four
legs and a gait stance are the start of the list, not the end of it.** What a
quadruped needs that neither of those has:

1. **Four limbs that reach the ground, on ground that is not flat.** A bird's two
   legs (`bird.py:1180`) are struts under a body whose height is free; a fish has
   none. A standing quadruped's foot positions are a *constraint* on the body
   transform, not an output of it, and the world it stands on is sloped.

2. **A stance, and the seed trap that comes with it.** Standing, walking,
   grazing (head down), alert (head up) and lying are five different rest
   configurations. A cubic voxel grid cannot store a rotation delta — **the grid
   IS the rest configuration** — so each pose is separately authored, exactly as
   `bird.pose` is. `docs/bird-shape-research.md` §10 records the bug that comes
   with this and it will recur verbatim: the pose is part of `spec.spec_hash`, so
   the same species in two poses came out as two *different animals* with
   different lengths and different markings. The fix was to seed from a hash that
   leaves the pose out (`spec.seed_hash`, `spec.SEED_INVARIANT`), with the field
   normalised to its default rather than deleted. **Anything that adds a second
   authored pose inherits this**, and a quadruped has five.

3. **A horizontal trunk with an independently angled neck, and a withers-to-hip
   height difference.** A bird's body is one tilted axis. A bison's shoulders are
   twice the height of its hips, a giraffe's neck is vertical off a horizontal
   back, a hyena slopes. This is two more angles and one more height ratio than
   the bird has, and they carry most of the between-species signal.

4. **A muzzle, which is not a bill.** `bird.py:817`'s `_bill` is a tapering cone
   projecting from the head. A muzzle is a box that continues the skull, with a
   jawline. Different primitive.

5. **Branching headgear.** Antlers are the genuinely new geometry — and probably
   not new code: `forge/skeleton.py`'s `grow` and `grow_whorl` already build
   branching skeletons with orders, radii and tapering, which is exactly what a
   set of antlers is. **A stag's antlers are a very small tree.** Horns are
   simpler still — a swept tapering cone, often with transverse ridging.

6. **Rod tails with an optional terminal tuft.** A bird's tail is a flat fan
   (`bird.py:904`). A quadruped's is a rod: a deer's 15 cm scut, a squirrel's
   full-length plume, a zebra's tassel, a kangaroo's counterweight thicker at the
   base than the animal's neck. Nothing in the library makes any of those.

7. **Ears.** A paired plate or cone on the skull. Neither the bird nor the fish
   generator has anything of the kind, and on a hare, a fennec fox, a kangaroo
   or an elephant the ears are the entire species.

8. **Markings that wrap a body rather than sit on a flank.** The fish and bird
   generators paint marks onto named regions. A zebra's stripes are transverse
   bands wrapping a cylinder — which **is** the fish generator's "vertical bars"
   mark, and should be reused, floor rules included. A giraffe's reticulation and
   a leopard's rosettes are not, and are new.

9. **Structural sexual dimorphism.** `bird.py:274`'s `_sex_scale` scales parts by
   a ratio. A stag against a hind is a part that is *present or absent*, which is
   a larger structural difference than any bird dimorphism in the library.

10. **A per-species lattice, which the spec schema already supports.** Fish and
    cetaceans carry `length_m` plus `resolution_cm` per spec; quadrupeds need the
    same, and they need it more, because the range runs from a 22 cm squirrel to
    a 3 m bison and the identifying features do not scale with the body (§5).

Everything above is geometry. **The largest practical risk is not geometry**: it
is that a quadruped is the first asset here that a player will judge as an
*animal*, against a lifetime of knowing what animals look like. `bird.py` is
1,535 lines and `fish.py` is 1,356; budget accordingly.

**Build the seal first.** A pinniped is the easiest possible shakedown for animal
machinery: no legs to place, no gait, one fused hind flipper, and a resting pose
that lies flat on the ground. It exercises the body loft, the muzzle, the pose
field and the per-species lattice, and none of the four-limb problem.

---

## 5. Two `hosts` findings

`hosts` (`biomes.py:30`, `52-82`) is the generator's own table of which asset
kinds may occur in which biome. It is not mirrored in the engine and nothing in
`voxel-core` reads it, so changing it is a one-line edit that needs no worldgen
version bump. Two biomes' tuples are narrower than their species lists:

* **Ocean hosts only `("fish", "cetacean", "bird")`** (`biomes.py:58`). No
  `rock`, no plant kinds. So the sea floor cannot carry a boulder, a reef, a kelp
  frond or a seagrass meadow — 15 entries in `00-ocean.md` are marked
  `host: rock` or `host: grass`/`host: reed` for this reason. This is the largest
  single gap in that biome.
* **Bare rock hosts only `("rock", "bird")`** (`biomes.py:81`), because
  `plantable` is `False`. That is correct for plants and arguably wrong for
  animals: the 35° gate is the angle of repose **for loose material**, which is
  not the angle at which a hoofed animal loses footing. Ibex, chamois, mountain
  goat and rock hyrax routinely stand on steeper. `09-bare-rock.md` records this
  as a request rather than a queue — those rows are double-blocked, on the
  `hosts` tuple *and* on the missing generator, so the tuple should not be widened
  until a quadruped generator exists and can fill it.

A third, smaller observation: `cetacean` appears in the hosts tuple of every
plantable biome including **desert**, because the tuple is shared
(`biomes.py:43`, `_SWIMS`). Harmless today, worth knowing if that tuple is ever
tightened.

---

## 6. The lattice rules, and the two ways a species fails them

The house rule, recorded at `forge/kinds.py:29-58` and measured in
`docs/marine-megafauna-research.md`: **a species is drawn at the coarsest voxel
size at which its smallest identifying feature is still about three voxels
across**, and a difference landing on fewer than two voxels reads as a mistake
rather than as a feature.

Which lattice is even available depends on the kind (`kinds.py:29-58`):

* **Terrain lattice — trees and rocks — is 10 cm and nothing else.** They join
  the world's own voxel grid and are destructible as terrain is; the grid has one
  cell size, `vxc::kVoxelSizeMm` = 100 mm, and there is no resampling anywhere in
  `voxel-core`. `forge.cli.selftest` refuses a terrain-lattice spec at any other
  size.
* **Detail lattice — everything else — is free.** Shipped practice: ground cover,
  bushes, flowers and reeds 5 cm; birds and small fish 1 cm; medium fish 2 cm;
  sharks and dolphins 5 cm; great whales 10 cm.

**Species fail this rule in two completely different ways, and the files keep
them distinct because the fixes differ.**

**(a) A fine feature drags the whole body to a finer lattice than its size
wants.** A squirrel is 22 cm and its tail is the whole silhouette; a wildcat's
tail rings are 3 cm bands; a hare's ears are 3–4 cm wide. The body would have
been happy at 5 cm; the feature forces 1 cm. This is fine — it costs voxels and
nothing else. Roughly 40 rows across the ten files are in this class.

**(b) The feature vanishes at every lattice the body can use.** A red deer stag
is 2 m and its antlers are the whole silhouette, but a main beam is 3–4 cm and a
tine tip 1–2 cm: at 5 cm the antlers disappear and the stag becomes a hind, and
at 2 cm a beam is two voxels and a tine one — still under the rule. **There is no
lattice at which life-size round antlers read.** Zebra stripes (5–8 cm on a 2.3 m
body), leopard rosettes, giraffe reticulation and reindeer brow tines are all in
this class.

**The fix for class (b) is already house practice and must be written down each
time it is used**: author the feature above life size and record the reason in
the spec's `notes`. Four birds in the library are authored at 20–26 cm against
real lengths of 14–19 cm, `clown-anemonefish` is at 22 cm against a real 10 cm,
and each says so in its own `notes` — because **a note is what stops the next
person "correcting" it back** and quietly destroying the asset.

The single most useful contrast in these files, and the one that decides build
order for deer: **palmate antlers survive a coarse lattice and round tines do
not.** A fallow deer's or a moose's antlers are flat 10–15 cm blades and read at
5 cm unaltered. Build those first.

---

## 7. Where to start

Priority across the whole world, not within one biome. The ordering weighs land
share against cost, and it deliberately puts everything that needs **no new
generator** ahead of everything that does.

1. **Grassland wildflowers and prairie grasses.** Grassland is **28.06% of all
   land** — more than a quarter of everywhere a player can stand — and it has two
   flower specs and three grasses. Poppy, cornflower, oxeye daisy, yarrow,
   knapweed, clover, thyme, big bluestem, feather grass. The tuft generator is
   shipped and proven, and each spec changes the colour of a whole hillside.
   Nothing else in this document costs so little per unit of visible change.
2. **Savanna ground layer.** 20.76% of land, second largest, and the same
   argument.
3. **Temperate forest floor.** Ferns and mosses. This is a *correctness* gap as
   well as a volume one: a temperate forest floor made of meadow grass is the
   wrong biome. Settle first whether a continuous 2–5 cm moss mat should be a
   terrain material rather than a scattered asset — that decision changes what
   gets authored.
4. **Beach dune grasses.** Beach is only 5.54% of land but it **wraps every
   coastline and every lake shore in the world**, so a player at any water's edge
   anywhere sees it. Bare sand is the most conspicuous emptiness in the world.
5. **Open-country and shore birds.** Lapwing, crane, white stork, pheasant, red
   kite, oystercatcher, curlew, avocet, gannet, cormorant, tern. The bird
   generator handles all of them, and wader identity lives almost entirely in
   **bill shape**, which `bird.bill_frac`, `bill_curve` and `bill_depth` already
   parameterise. Many species for very little new geometry.
6. **The emu, on its own and out of order.** A large, unmistakable,
   animal-shaped entity that needs **no new generator** — it is a big flightless
   bird. It is the cheapest possible partial answer to "the world has no
   animals". The greater rhea is the same trick again.
7. **Pelagic shoal fish.** Mackerel, sardine, cod, sea bass. The fish generator's
   core case, and the schooling fields in the `detail` group already exist, so
   one spec becomes many entities.
8. **Test the serpentine hypothesis** (§3). One experiment: does the fish
   generator with fins off make a snake? If yes, nine rows unlock for almost
   nothing, and the result should be known before the quadruped work is scoped.
9. **The quadruped generator** (§4) — the largest single unlock in the library at
   167 rows. Build a seal first as the shakedown, then in this order:
   **bison** (largest, most iconic, simple horns), **fallow deer** (palmate
   antlers survive 5 cm, spots are a shipped-style marking), **wild boar** (no
   fine appendages to lose), **red fox**, **zebra** (the first species whose
   *marking* sets the lattice), **grey squirrel**. Leave the red deer stag and
   the elk until the antler-thickness question in §6 has a rendered answer.
10. **Then the ocean `hosts` change** (§5), which unlocks reefs, kelp and
    seagrass. Last only because it is the one item that needs someone else's file
    changed, and because the nine above do not.

---

## 8. Where the numbers come from

**Every species size in these ten files is an approximate typical adult figure
from general knowledge. None of it is measured, and none of it is sourced.** It
is good enough to choose a voxel lattice, which is what these documents are for,
and it is **not** good enough to quote as a fact or to paste into a spec's
`notes` without checking first.

This is stated at the foot of every file, and it is stated because **this project
has already shipped a fabricated citation**: an orca eye patch was documented as
"21.8 × 5.9 cm, aspect 3.7:1, measured" in three separate places, and the figures
turned out to be two dimensionless indices lifted from an unrelated paper. A
second agent later found the identical trap in a different source — a
morphometrics table printing percentages under headings that read like
millimetres. So these files **cite nothing**. There are no papers, no URLs and no
datasets anywhere in them. The only references are repo paths and line numbers,
and every one was opened and read.

Note that this is a **deliberate departure from the house standard** set by
`docs/fish-shape-research.md` and `docs/bird-shape-research.md`, which cite
heavily and correctly. Those documents answer "what shape is a fish" and needed
sources. These answer "which species belong here and roughly how big are they",
and the honest position for that question, with no search budget available this
session, is an explicit blanket approximation rather than a sprinkling of
plausible-looking decimals.

**What in these files is exact:**

* Every `biome.h`, `biomes.py`, `kinds.py`, `worldgen.ush` and spec path and line
  number.
* Every shipped spec name and its authored size, read from the JSON
  (`height_m` for plants, `length_m` for animals, `block_size_m` for rocks,
  `resolution_cm` for the lattice).
* The land shares, taken from the **289-tile column** of
  `docs/measurements/biome-screenshot-targets-2026-08-01.txt`.

**What is estimated, and where:**

* **All species sizes.** As above.
* **The Lattice column on every row that is not `shipped:`.** These are
  recommendations derived from the three-voxel rule applied to an estimated
  feature size. Only shipped rows have a lattice that has actually been tested.
  Confirm with `tools/fishprobe.py --lattice` or `tools/birdprobe.py --lattice`
  before writing a spec, exactly as the fish and bird research did for their sets.
* **The lattice arithmetic in §6 and in each file's ⚠ notes.** The conclusion
  that round antler tines fail the rule at both 5 cm and 2 cm while palmate
  antlers pass at 5 cm is arithmetic on estimated thicknesses. It should be
  checked by rendering one before it is used to reorder anyone's work. The same
  applies to the zebra-stripe and leopard-rosette widths, which drive six lattice
  choices in `06-savanna.md` and are the weakest numbers in that file by its own
  admission.
* **The generator verdicts in §3** — serpentine, succulent, coral, lichen — are
  arguments, not findings. Each says which existing generator comes closest and
  why. None has been tried.
* **Biome assignment at the margins.** Flagged in-row wherever it bites, rather
  than smoothed over: grassland is a catch-all spanning cold steppe to
  Mediterranean scrub and several of its animals would sit elsewhere under a finer
  classification; `hero-sequoia` and `columnar-cypress` are weighted to taiga and
  are not boreal trees; six of seven shipped savanna birds are Palearctic; the
  alpaca is domesticated and the vicuña and guanaco are the honest wild entries;
  the small Indian mongoose and the western mosquitofish are introduced almost
  everywhere they are common. **The biome weights on a spec, not these files'
  headings, are the final word.**

**One correction made while writing these files, worth knowing about.** The
census file above contains two censuses. The first, over 121 tiles, is quoted
widely and is **superseded by a 289-tile rerun in the same file**, which carries
two explicit corrections written by its own author. The shares moved by up to
6.7 points — tundra/alpine from 26.34% to 19.62%, temperate forest from 2.43% to
4.90% — and two confident causal explanations built on the smaller sample were
retracted as sample bias. Every share in these files is the 289-tile figure, and
the biome files say so where the older number is likely to be met elsewhere. If
you are quoting a biome share from anywhere in this repo, **check which census it
came from.**
