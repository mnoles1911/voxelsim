# Trunk taper, and the five other things the plant fit deferred

Written after the pass that closed out the generator limitations
`docs/plant-proportion-research.md` §5.4 and `docs/biomes/README.md` §9.3 found,
measured and deliberately left alone. Six items came in. **Three became
generator terms, one became a menu, one became a lowered bound, and one is
answered NO with the measurement that says so.**

Every number here was measured on this machine unless it is marked
`[estimate]`. The renders are listed at the end and no verdict is offered on
them.

---

## 0. The one-line summary of each

| # | The limitation | What happened |
|---|---|---|
| 1 | No trunk taper at all; `birch` measured 1.00 on three seeds | **Built.** `trunk.taper`, a ceiling on Murray's law. 41 of 78 species thin at quarter height, median 7.3%; form quotient 0.848 → 0.707 |
| 2 | Root flare scales with height, swallows breast height on 50 of 73 trees | **Built.** Bounded in metres, below 1.3 m, with an exception for stems wider than that |
| 3 | `envelope.py`'s hardcoded 0.85 "not a measurement" | **Removed.** It now calls the taper curve. No spec in the library uses allometry, so the effect on the library is exactly zero and is verified by unit rather than by sweep |
| 4 | Bush generator floors at about 0.5 m | **Lowered**, after measuring that the floor is the LATTICE and not the plant |
| 5 | `materials.head` and `materials.stem` are different menus, silently | **One menu**, asserted at import in the file that resolves both |
| 6 | `trunk.lean_deg` caps at 40°, so a trunk cannot lie down | **NO — and raising the cap is worse than useless.** At 90° the asset comes out MORE vertical than at 40° |
| + | `resolution_cm` out-of-menu silently becomes 5 cm (coordinator, mid-pass) | **Folded into 5.** All 70 choice menus are now tripped every selftest run, and the false help text is rewritten against the measured library |

---

## 1. Taper: the term that was not there

### 1.1 What was wrong

`skeleton._radii` derived every node's radius from Murray's law over the
branching topology and rescaled the whole tree so the root matched
`trunk.radius_base_m`. Thickness was therefore a consequence of **where branches
fork** and of nothing else, so a stem that had not forked yet kept its base
radius all the way up. `birch` measured a taper ratio of 1.00 on all three
seeds; `hero-sequoia` measured 0.977.

The best evidence that this was a real gap and not a theoretical one is that a
spec author had already hit it and worked around it. `tree-fern`'s own notes:

> **NO TAPER IS THE SPECIES.** A tree fern's trunk is the same width top to
> bottom — it is a mass of old frond bases, not wood — so `radius_exp` is 3.5,
> the top of the range, which keeps the parent thick all the way up, and the
> root flare is zero. **Everything else here tapers.**

Everything else here did not taper. The author reached for a branch-thickness
exponent to say "no taper" because there was no way to say it, and believed the
default behaved the other way.

### 1.2 What was built

    r(z) <= trunk.radius_base_m * ((H - z) / H) ** trunk.taper

applied as a **ceiling** on the radius Murray's law already produced, floored at
`growth.tip_radius_m`. Three properties follow from that shape and each one is
the reason for it:

* **It can only remove wood.** A multiplier would thin the crown as well, where
  the branching model is already doing the job correctly, and pinch every twig.
  The minimum bites only where Murray's law says "cylinder", which is the
  unforked bole.
* **It cannot eat a twig.** The envelope goes to zero at `z = H`. Without the
  floor, the leader of a whorled conifer tapers away to nothing and takes the
  foliage anchored on it with it.
* **`trunk.taper` 0 is an exact no-op**, because `radius[0]` is the largest
  radius in the tree by construction, so the minimum is a no-op everywhere.
  That is what makes the before/after in §1.4 a real A/B rather than a rebuild.

The exponent is the classical solid of revolution: 0 cylinder, 1/3 Metzger's
cubic paraboloid, 1/2 paraboloid, 1 cone. The default is 0.5. **It is not
fitted to anything**, because §8 of the plant research says no taper equation
was obtainable licence-clean and that is still true. What has changed is that
there is now somewhere to put one.

At this lattice the second decimal is not worth arguing about: on a 20 m tree at
half height, exponents of 0.4 and 0.6 differ by 0.758 against 0.660 of the base
radius, which on a 40 cm stem is under half a voxel.

### 1.3 Seven species author 0, and they are all the same botany

Palms, a tree fern, a banana, a screwpine and a saguaro: monocots and a fern
have no vascular cambium, so the trunk is laid down at full width and never
thickens. `coast-palm`, `date-palm`, `doum-palm`, `tree-fern`, `wild-banana`,
`screwpine` and `saguaro` carry `trunk.taper` 0 with the reason in their notes.
`joshua-tree` was left at the default: it has anomalous secondary thickening and
its trunk does taper.

### 1.4 What it measured

`tools/trunkform.py`, which is new, walks the stem up the built voxels and
calipers it at 0.1 m, 1.3 m and at 25%, 50% and 75% of the tree's own height.
It exists because `plantprobe`'s `taper` is d(1.3)/d(0.1), the bottom 6% of a
20 m tree, and a taper term that is doing its whole job up the bole moves that
number by about 3% — read alone it would have said this change does nothing.

**78 species × 3 seeds, each against ITSELF with the term off.** Taper is in
`spec.SEED_EXCLUDED`, so both sides are the same individual.

| | taper off | taper on |
|---|---|---|
| **form quotient d(H/2) ÷ DBH**, median of 234 builds | **0.848** | **0.707** |
| d(1.3) ÷ d(0.1), median | 0.782 | 0.769 |

Per species, medians over three seeds:

* **d at quarter height fell on 41 of 78 species, by a median 7.3%.**
* **Diameter at breast height barely moved: median 0.0%, only 19 species over
  2%.** That is the design working — the ceiling bites up the bole, and the
  number the library is fitted against is nearly untouched.
* **No species gained a single voxel.** Median −0.2%, range −16.2% to +0.0%,
  which is the ceiling-not-multiplier property holding across the library
  rather than being asserted. Largest: `hero-sequoia` −16.2% (4,079,524 →
  3,419,277), `baobab` −11.3%.
* Built height median +0.0%, worst +1.0%.

**The 36 species that did not move are the thin ones, and that is the lattice,
not the model.** Their median DBH is 44 cm against 66 cm for those that did: at
10 cm a 2.5-voxel stem cannot lose 10% of its radius without losing a whole
voxel. `birch` still measures a taper ratio of 1.00 and now does so for a
reason that is written down — a 25 cm bole is 2.5 voxels and there is no
thinner cylinder available on the terrain lattice. §5.4's own caveat, from the
other direction.

---

## 2. The root flare, and why the fit needed four passes

`trunk.buttress` spent its falloff over the bottom **eighth of `height_m`**. On
a 24 m beech that is 3 m, so breast height — 1.3 m, where every published trunk
diameter is measured and where `plantprobe` measures ours — sat **inside the
flare on 50 of the 73 trees that author one**. A control for how a tree meets
the ground was acting as a control on the number the library is fitted against,
and every pass of `plantfit` moved the base radius by a factor the flare made
non-linear.

The reach is now bounded in metres as well: `min(H/8, max(1.2 m, one stem
diameter))`.

* **1.2 m is not a measurement.** It is the largest round number that still
  leaves breast height outside the flare, and that property — that
  `trunk.radius_base_m` and the fitted DBH are two independent numbers — is the
  whole point of it.
* **One stem diameter** is the exception for stems too wide to spend their swell
  in a metre `[estimate — no butt-swell reach was sourced]`. Only
  `hero-sequoia` (8.4 m of the 10 it had), `kapok` and `jungle-emergent` (1.7 m)
  are above 1.2 m; every other tree in the library lands on 1.2. Those three are
  also exactly the species a forester measures above the buttress rather than at
  1.3 m.
* Nothing under about 9.6 m of height changes at all, because `H/8` was already
  the smaller term.

**What it did, measured over all 78 trees at three seeds each, before against
after:**

| | median | species moving >2% |
|---|---|---|
| **d at breast height (1.3 m)** | **−3.4%** | 44 thinner |
| **d at the base (0.1 m)** | **+0.0%** | 9 thinner |
| d(1.3) ÷ d(0.1) | 0.809 → **0.777** | — |

That pair of rows is the whole result. **The flare at the ground is untouched
and the flare at breast height is gone**, which is exactly what was asked for:
the tree still stands in the ground, and the number a forester measures is no
longer partly a buttress setting. The taper ratio moves toward the 0.7–0.9 band
`plantprobe`'s own docstring gives for a real stem. The largest movers are the
species that authored a big flare on a tall stem — `western-red-cedar` 136.8 →
106.5 cm, `kapok` 183.6 → 150.2 cm, `sitka-spruce` 85.8 → 74.4 cm — and their
base diameters are unchanged to the voxel.

Taper and flare are separable in this measurement: the taper A/B above moves DBH
by a median 0.0%, so essentially all of the −3.4% here is the flare bound.

---

## 3. The 0.85 that was "not a measurement"

`envelope.apply_allometry` converted base radius to DBH with a hardcoded 0.85,
its own comment calling it "a taper allowance, not a measurement". It now calls
`envelope.taper_factor`, the same curve `skeleton._radii` applies, so the crown
allometry is told what this tree's stem actually does. A palm at `trunk.taper` 0
is correctly told its stem is the same width at 1.3 m as at the ground, which
the constant could never say.

**What moved in the library: nothing, and that is a measurement, not a hope.**
All 828 specs author `crown.allometry: off`, so this code path is not reached by
any species today. A library sweep would therefore have reported "no change" for
a correct fix and for a deleted one alike, so the check is a direct one on the
function instead.

---

## 4. The bush floor is a lattice floor

Four parameters put a half-metre minimum under a kind whose lattice is free:
`crown.radius_m` 0.30, `growth.influence_m` 0.40, `growth.step_m` 0.08,
`trunk.radius_base_m` 0.05 — plus `growth.kill_m` 0.10, which
`docs/biomes/README.md` §9.7 records separately. On a 25 cm plant the
crown-radius floor alone is 1.2× the whole plant's height.

Measured, one spec, five seeds each, against the 94–185 voxel band a shipped
small tuft occupies:

| | voxels, seeds 1–5 | fork order |
|---|---|---|
| 0.60 m at 5 cm, as `sea-oak-weed` ships | 212–246 | 3–4 |
| 0.25 m at 5 cm, floors as they were | 66–89 | 2–3 |
| 0.25 m at 5 cm, floors lowered | 45–55 | 2–4 |
| **0.25 m at 2 cm, floors lowered** | **171–196** | **3–5** |
| 0.25 m at 1 cm, floors lowered | 626–660 | 5–8 |

So the floor was never a floor on the plant, it was a floor on the **lattice**.
Eight growth steps of 8 cm is 0.64 m, and eight steps is about what it takes to
reach a fork — which is why `sea-oak-weed` at 0.60 m is the smallest bush in the
library that works, to within a voxel of the arithmetic. Bring the lattice down
with the plant and the same species builds in the shipped band **with** the
dichotomous forking that moving it to a tuft threw away.

The floors are lowered to what the finest legal lattice can carry. What stops
nonsense now is `validate`'s existing cross-checks — `kill >= influence` warns
"the tree will be a bare trunk", which is the exact message `dead-mans-fingers`
produced — and those are relationships between the three numbers, which is what
actually governs whether colonization can branch. A fixed metre bound never was.

**The runaway these might be imagined to guard does not exist**: a 30 m beech at
`step_m` 0.02 measured 10.0 s and 7,227 voxels, i.e. it degenerates into a
scribble at the same cost as any other bad number rather than exploding.

**The five aquatic species were left as tufts.** They build in band today and
re-authoring five shipped species is a change the owner should see rather than
inherit; the measurement above is what that decision needs.

---

## 5. One material menu

`materials.stem` offered seven land-vegetation greens and browns,
`materials.head` fourteen, overlapping in four. A head material outside its own
menu is not refused, it is **replaced with the default**, and the default is
`leaf_blossom`, a pink at (226,168,190). Four freshwater plants shipped that
way — a brown-cigar cattail, a black-spiked sedge and two sets of lotus pads,
all in blossom pink, all validating clean and building clean.

**Where the silence actually lives is worth being precise about**, because the
obvious fix does not work. `validate` DOES warn. But the substituted value is
what gets saved, so from that moment the file on disk is self-consistent and no
gate downstream has anything to find: `tools/buildcheck.py` already fails on any
spec warning at load and could not have caught this. The window between
authoring and saving is the entire window.

So the fix is not a louder warning:

* **One tuple**, `spec._PLANT_MATERIALS`, read by both rows. An author who picks
  a stem colour for a head can no longer be substituted, because it is the same
  menu.
* **`forge/ground.py` asserts at import** that the two rows still share it —
  the same guard `envelope.py` puts on crown shapes and `rasterize.py` on
  foliage habits, written after the same class of accident.
* **The warning names the consequence** — that the asset will be *built and
  drawn as* something else and that nothing downstream will report it again.
* **`selftest` exercises the alarm rather than trusting it.** A bogus choice is
  pushed through five different menus every run and the gate fails if any of
  them accepts it quietly. A silent substitution is invisible by construction,
  so the only way to know the alarm still works is to trip it on purpose.
* **`selftest` also re-validates every spec from disk**, which catches a
  hand-edited file — a real authoring route, per §9.6 of the biome list.

The union is additive: no default moved, no choice was removed, no spec's
canonical JSON changed. It also answers §8.6a's two other complaints — `podzol`
gives `head` the dark brown it had none of, and `skin_red` is added for the dark
red that cost `marsh-cinquefoil` its identity. **Neither species was
re-authored**; the menu is the generator fix and the colour is the owner's.

---

## 6. A trunk still cannot lie down, and the cap is not why

`trunk.lean_deg` caps at 40°. Horizontal is 90°. The obvious reading is that
raising the cap unblocks **fallen mossy log** and **driftwood snag**. It does
not, and the measurement is unambiguous. `desert-dead`, two seeds, lean forced
past the cap in memory:

| lean | bbox X × Y × Z (m) | longest ÷ tallest | health |
|---|---|---|---|
| 0° | 4.4 × 3.7 × 6.4 | 0.69 | clean |
| 40° (the cap) | 4.6 × 4.9 × 5.0 | 0.98 | clean |
| 60° | 4.9 × 5.7 × 5.3 | 1.08 | clean |
| 75° | 4.7 × 5.4 × 7.0 | 0.77 | clean |
| **90°, horizontal** | **5.2 × 5.6 × 6.1** | **0.92** | clean |
| 90° with `trunk.wander` 0 | 3.7 × 3.7 × 6.6 | **0.56** | clean |

**A trunk authored horizontal produces an asset MORE vertical than one authored
at 40°**, and `pipeline.health` reports it clean every time. Three separate
mechanisms hold it upright and the angle is not among them:

1. `skeleton.grow`'s trunk loop applies `nd[2] = abs(nd[2]) + 0.25` on every
   wandering step — "never let the trunk turn back downward" — which erases the
   authored lean on the first step for any species with `trunk.wander > 0`.
2. The trunk loop **breaks as soon as its tip can see a growth target**. At 90°
   the tip travels one 0.24 m step sideways, is immediately within the 2.2 m
   influence radius of a crown envelope that sits 1.6–5.8 m up over the origin,
   and stops. The tree is then its crown, and the crown is a vertical volume
   over the base whatever the trunk did.
3. Root flare, fluting, the "never grow into the ground" test and ground contact
   are all keyed on z.

So a lying trunk needs the whole asset laid over — a rotation of a finished
grid, at placement or at export — and not a growth angle. Both biome files
already say the answer is `desert-dead` rotated, and that remains right. Raising
the cap would have shipped a knob that reports success and does nothing, which
is the exact failure this project keeps producing.

---

## 7. Does the trunk-diameter fit still hold?

**Yes, loosened by about a point and a half, and every bit of the loosening is
the flare bound rather than the taper.** Same 52 species, same method as §5.2,
three seeds, measured against the same open-grown target:

| | n | median ours ÷ target | mean abs error | within 10% | within 25% |
|---|---|---|---|---|---|
| before this pass | 52 | 1.02× | 19.9% | 38/52 | 45/52 |
| **after** | 52 | **0.97×** | **21.6%** | 30/52 | 44/52 |

`tools/plantfit.py report` on the same library says 30 of 52 in band, 16 would
move, 6 are already at the one-voxel floor and cannot. The direction is
consistent: trees are now a few percent thinner at breast height, because the
buttress is no longer being measured as stem.

**The fit was NOT re-run, on purpose.** Two reasons, and the owner should
overrule either if they disagree:

1. It is a **second opinion on 16 shipped species** four passes after the last
   one, and a median of 0.97× is not a defect — it is a third of the distance
   the quantisation of a 10 cm lattice moves a thin stem anyway.
2. `plantfit fit --apply` writes through `spec.patch` + `spec.save`, and §8
   below shows that **that combination silently reseeds any spec missing
   `quad.eye`** — 8 of the 78 trees. Re-fitting today would change 16 trunk
   radii and, invisibly, up to 8 individuals. That should be decided after the
   bug is, not before.

One command, whenever that is settled:

    python tools/plantfit.py fit --apply --seeds 1 2 3

---

## 8. Verification

    python -m forge.cli selftest    -> selftest: PASS   (824 specs, 828 re-validated)
    python tools/buildcheck.py      -> buildcheck: PASS (828 builds in 774 s)
    python tools/buildcheck.py --kind tree --seeds 1 2 3 4
                                    -> buildcheck: PASS (312 builds in 286 s)

Both gates match the baseline exactly: 824 specs and 828 builds. The four-seed
tree run is the one that matters for this change, because everything here
changes tree geometry and a defect on one individual in eight is invisible at
seed 1 — which is how the kelp and sequoia bugs survived.

**Nothing in the library was reseeded**, which is a claim and therefore a
measurement: `python tools/trunkform.py --hashes out/taper/hashes-before.json`
reports 0 of 828 seed hashes moved by this pass. (81 quadrupeds moved during it,
all of them edited on disk by the concurrent animal work, and the tool names
them so the two can be told apart.) Spec hashes all moved, which is expected and
correct on any `PARAMS` change: that is identity, not individual.

Artefacts, all under `out/taper/`:

| file | what |
|---|---|
| `trunkform-ab.txt` / `.json` | 78 trees × 3 seeds, taper off against taper on, same individual |
| `tree-before.json` / `tree-after.json` | `plantprobe --kind tree --seeds 1 2 3`, whole pass before and after |
| `plantfit-after.txt` | `plantfit report`, per species |
| `selftest-after.txt`, `buildcheck-after.txt`, `buildcheck-tree-4seed.txt` | the gates |
| `hashes-before.json` | the pre-change seed hashes the no-reseed claim is checked against |

**Renders. The owner judges these; no verdict is offered.** Each pair is one
locked scale, computed from the larger of the two builds and applied to both,
bottom-aligned on a common ground line — `tools/tapersheet.py`, which says why
in its docstring.

| file | what |
|---|---|
| `ab-seed1.png`, `ab-seed3.png` | 12 species, before over after, two seeds |
| `ab-hero-sequoia.png` | the largest single change, 5,070,887 → 4,654,138 voxels |
| `bush-floor-ab.png` | `dead-mans-fingers` as it ships (tuft, 5 cm, fork order 0) against the same species as a bush at 2 cm with the floors lowered (order 3–7) |

The twelve are chosen to cover the range and to include the cases where nothing
should change: `coast-palm` and `tree-fern` are byte-identical on both sides,
which is the `trunk.taper` 0 opt-out working, and `birch` differs by 5 voxels in
72,364, which is the lattice.

---

## 9. A bug found while measuring, reported and NOT fixed

**`spec.validate` is not idempotent, and an empty patch changes which individual
you get.**

`quad.eye` is declared `kind="int"` with a default of `1.0`, a float. A spec
that does not carry the key on disk keeps the float through `load`; the next
validate coerces it to `1`. The canonical JSON changes from `"1.0"` to `"1"`,
and the canonical JSON is what `seed_hash` hashes. So:

    s, _ = sm.load("specs/hawthorn-scrub.json")
    s2, _ = sm.patch(s, {})          # an EMPTY patch
    sm.seed_hash(s) != sm.seed_hash(s2)     # True

**191 of 828 specs on disk are missing the key** — 52 flowers, 44 grasses, 34
rocks, 20 birds, 12 fish, 10 reeds, 8 trees, 7 cetaceans, 4 bushes. Anything
that patches and saves has been quietly reseeding them, which includes
`tools/plantfit.py fit --apply`.

It cost this pass a full sweep: `hawthorn-scrub` came back 9,726 voxels against
18,274 and read as a large taper effect on a species where taper does nothing at
all. `tools/trunkform.py` now patches both sides of its A/B so both go through
the same number of validations, and the sweep was re-run.

**Not fixed here on purpose.** Every available fix moves those 191 specs to
different individuals, `quad.eye` belongs to the animal work, and a
library-wide reseed is the owner's call and not a side effect of a taper pass.
