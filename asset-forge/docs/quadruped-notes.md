# Land animals: what was built, what was decided, and what is still open

Companion to `forge/quadruped.py`. The generator's own header explains how it
works; this records the decisions that needed an argument, the two material
proposals that are **not** being made yet, and an honest list of what does not
work.

---

## 1. The kangaroo is a stance, not a limb count

**`quad.stance` is a choice with three values: `standing`, `sprawling`,
`bipedal`.**

A kangaroo stands on two hind legs and a heavy tail as a tripod, with small
forelimbs held clear of the ground. A monitor lizard stands on four limbs held
*out to the side* with its belly a few centimetres up. Neither is reachable by
any setting of a generator that assumes four limbs directly under a trunk.

Three designs were possible and the other two were rejected:

**A limb count** — "a kangaroo is a biped". This is false in the way that
matters. It has four limbs, the forelimbs are visible at any distance the animal
is, and they are part of what makes the silhouette read as a kangaroo rather
than as a bird. A limb count of two deletes them. What is actually different is
*which limbs touch the floor*, and that is a property of the stance.

**Three continuous knobs** — how far the forelimb reaches, whether the tail is a
support, where the forelimb attaches. Only one combination of the three is an
animal, and the other seven are a spec nobody would notice was wrong. This is
the argument `bird.pose` already settled: when the combination is the thing, name
the combination.

`sprawling` would have needed to be a choice regardless. A sprawled limb leaves
the **flank**, not the belly, so the attachment point moves and no angle on a
limb hanging under the body reaches it.

### The seed trap that does not arise

`docs/biomes/README.md` §4.2 predicted that a quadruped would inherit the bug
`bird.pose` hit: the pose is part of `spec.spec_hash`, so the same species in two
poses came out as two different animals.

It does not arise, and the reason is worth stating so nobody adds
`quad.stance` to `spec.SEED_INVARIANT` later. **A stance is a species property,
not a posture.** A kangaroo cannot stand quadrupedally and a crocodile cannot
stand like a horse. It is the same kind of field as `fish.caudal_plane`, and it
belongs in the seeding hash. Animals ship in one pose (owner, 2026-08-14), so
there is no second authored posture for the trap to bite on at all.

### What `bipedal` actually does

1. The forelimbs attach at the **front of the chest** rather than under the
   shoulder, and are drawn at `quad.fore_reach` of the distance to the ground.
2. The tail's carriage angle **stops being authored and is solved** so the tip
   lands on the ground plane. A third leg that does not reach the floor is not a
   third leg.
3. Nothing else. The steep trunk comes from `quad.shoulder_h` and `quad.hip_h`
   like every other species — a kangaroo is a hip near the ground under a
   shoulder well above it, which is the same two numbers a bison uses in the
   other direction.

---

## 2. Should the seal have been built first?

`docs/biomes/README.md` §4 recommends building a pinniped as the shakedown: no
legs, no gait, one fused hind flipper, resting flat. **This was considered and
not done, and the reasoning is against the recommendation rather than around
it.**

The survey's argument is that a seal exercises the body loft, the muzzle, the
pose field and the per-species lattice with none of the four-limb problem. That
is exactly right, and it is the objection: **the four-limb problem is the risk.**
Everything else on that list is machinery the fish and bird generators already
proved. What had never been done here was solving a body transform against a
ground plane, and the two defects that actually cost time in this build were
both in it — a tail base placed outside the body, and a kangaroo's forelimbs
attached in front of its own chest, each shipping as a separate connected piece.

A seal would have found neither, and it is a different `kind` (`pinniped` in the
gap table), so building it first means writing a second generator's wiring before
the first one is proved.

What was done instead is the same de-risking inside one generator: the trunk,
neck, head and muzzle were built and rendered before the limb code existed, and
`tools/quadprobe.py --stance` was written before the stances were tuned.

The second reason is the owner's: the ask was **"I want to see a kangaroo"**, and
the kangaroo is the hardest case for limb placement in the whole queue. Deferring
limb placement defers the deliverable.

---

## 3. Lattice choices, and the trap in the rule

The house rule (`docs/marine-megafauna-research.md` §5.2): **the coarsest voxel
size at which the species' smallest identifying feature is still about three
voxels across.**

Every species in the tranche records its own arithmetic in its spec `notes`.
What came out, measured by `python tools/quadprobe.py --lattice`:

| lattice | species | body length in voxels |
|---|---|---|
| 1 cm | grey squirrel, hare, meerkat | 29–64 |
| 2 cm | 18 species | 49–150 |
| 5 cm | bison, brown bear, moose | 35–51 |

The band is **29 to 150 columns**, against the fish library's measured 28 to 294.
It sits at the low end of that because no land animal in this tranche is a blue
whale.

**The trap, met once and worth writing down.** The rule says the coarsest size at
which the FEATURE reads. It is easy to read it as the coarsest size the BODY can
tolerate, which is a different and much coarser number. The alpaca was authored
at 5 cm on the reasoning that nothing about an alpaca is small; that put a 1.4 m
animal on 28 columns — the bottom of the entire library's range, below every fish
in it — with its ears one voxel wide. It is at 2 cm now.

**The one species with no honest lattice** is the red deer stag, exactly as the
survey predicted. A red deer's main antler beam is 3–4 cm and a tine tip 1–2, so
at 5 cm the rack disappears and the stag becomes a hind, and at 2 cm a beam is
two voxels and a tine one. The spec authors `quad.horn_thick` at 0.13 against a
life-size figure near 0.07 and **says so in its own notes**, which is the house
fix for this class and the note is what stops the next person correcting it back.

---

## 4. Materials: two proposals, deliberately not made

**Nothing new is proposed and nothing new is used.** All twenty-four species are
authored out of the twenty-one creature materials already in the engine
(ten `skin_*`, ten `plume_*`, `beak_horn`). `python -m forge.cli selftest`
confirms every asset's materials exist in the engine.

Two colours the mammal set genuinely wants are recorded here, costed, and **not
requested** — the shapes should be approved before an engine append is spent.

### 4.1 `fur_grizzle` — a desaturated mid grey-brown

The gap is between `skin_brown` (110, 82, 52), which is a warm chocolate, and
`plume_grey` (150, 156, 164), which is a cool neutral. A wolf's saddle, a
badger's flank, a boar's bristle and a hyena's ground colour all live in
between, and every one of them is currently authored as one or the other.

Measured as WCAG contrast against what it would sit next to:

| pair | ratio |
|---|---|
| `plume_buff` vs `plume_grey` (what the wolf uses now) | 1.34 |
| `plume_buff` vs `skin_brown` | 2.72 |
| `plume_white` vs `plume_grey` (what the wolf uses after this pass) | 2.40 |

The wolf was re-authored to `plume_white` underparts to clear the 1.8 floor
`quadprobe --read` gates on. That works and it is slightly wrong — a wolf's
chest is cream, not white. **This is the weaker of the two proposals.**

### 4.2 `fur_black` — a true matte black

`skin_dark` is (46, 48, 56), a blue-black chosen for fish eyes and stripe work.
A gorilla, a black bear, a wild boar and a zebra's stripes are all authored in it
and it reads slightly cold on a large matte animal. This is a *look* argument
rather than a contrast one and it is the first entry to cut.

### What an append costs, every touchpoint named

`forge/materials.py`'s own header lists five tails, and this is them:

1. `vxc::Material` in `voxel-core/include/voxelcore/core.h` — append only.
2. The `static_assert` on the array length in `VoxelAgentSubsystem.cpp:64`.
3. The count assertion in `test_assetgrid.cpp`.
4. The positional palette table in `materialpalette.h`, order checked against
   the enum by `tools/gen_palette.py`.
5. **`ue-project/Tools/terrain_palette.py`, which refuses to generate until each
   new row has a `BIOME_TINT` decision.** This has ambushed two proposals
   already; a request that does not arrive with the tint decision already made
   is a request that stalls.

Plus the two generated mirrors — `forge/palette.py` and
`VoxelMaterialPalette.ush` — each with a check that fails if it was not
regenerated, and the four appearance numbers every material carries (per-voxel
lightness jitter, warm/cool tilt, patch strength, patch wavelength). Fur wants
the same end of every range the skins and plumage asked for: low jitter, low
patch strength, short wavelength. An animal is one smooth creature, not a
granular surface.

**No engine change has been made and none is requested here.**

---

## 5. What is unfinished, and what is uncertain

### Rosettes and reticulation are not implemented

`quad.mark` covers `bars`, `spots`, `saddle`, `flankstripe`, `dapple` and
`blotch`. A **leopard's rosette** is an annulus with a tawny centre and a
**giraffe's reticulation** is a partition of the flank into plates separated by
narrow lines. Neither is any setting of the six that exist, and the survey (§4.8)
says so.

The consequence today: `bengal-tiger` is right (its stripes are bars), and a
leopard, a jaguar or a cheetah authored now would carry plain spots. That is a
worse animal, not a broken one, so no leopard is in this tranche.

A rosette is probably a blob field drawn twice — once at the marking colour and
once at a smaller radius in the base colour — and it should be tried before a
Voronoi partition is written for the giraffe.

### The `hosts` tuple for bare rock was not widened

`docs/biomes/README.md` §5 argues that bare rock should host land animals: the
35° gate is the angle of repose *for loose material*, which is not the angle at
which an ibex loses its footing, and those rows are blocked twice over. The
argument is now half-answered — the generator exists — but widening the tuple is
a **placement** decision about where animals may stand in the world, not a
consequence of a generator landing. `forge/biomes.py` records this in a comment
and leaves it alone. It is a one-line change when someone decides.

### Things that are approximate and should be checked against a render

* **Every species size and feature size in the specs.** These come from the
  biome files, which state plainly (§8) that none of their numbers is measured or
  sourced. They are good enough to choose a lattice. They are not good enough to
  quote.
* **The zebra's stripe width**, which decides its lattice, is flagged by
  `06-savanna.md` as one of its own weakest numbers.
* **Whether the branched rack reads at all** at the authored thickness. The
  answer is a render of `red-deer-stag` beside `moose`, and the owner's, not
  mine.

### Known rough edges in the geometry

* **The feet are simple.** A hoof, a paw and a pad are all the same primitive at
  three sizes. A reindeer's splayed hoof and a wolverine's huge paw are both
  `quad.foot` and nothing else.
* **The forelimb is two segments and the hind limb three.** That is anatomy, but
  the fore limb of a knuckle-walking ape really wants three too, and a gorilla's
  arm currently bends once.
* **`quad.mane` is one ridge along the neck.** A lion's mane surrounds the whole
  head and a hyena's runs the length of the spine; both are authored as the neck
  ridge and neither is right.
* **The muzzle bend is three straight segments.** At three it is visibly
  faceted on a moose at 5 cm. More segments cost a full grid pass each.
