# The library stood a third too tall, and it was a definition error

Companion to `tools/refstance.py`, `refs/withers-target.json` and
`tools/refsil.py`. Written 2026-08-15, continuing
`docs/reference-fitting-research.md`, whose §7 OPEN ends: *"Belly clearance is
26% too high across the library and was not fitted."* This file is that fit.

**What the previous pass could see and could not explain.** Measured against 80
species of CC0 reference silhouette, the library's belly clearance over body
length ran **1.26× life**. That was the largest single finding in the reference
work and it was left alone deliberately, because the two knobs that move it
(`quad.shoulder_h` and `quad.depth`) both move the girth gate.

**What it actually was.** Not a tuning error. `quad.shoulder_h` is documented as
the height of the **shoulder joint**, and every value authored into 131 specs is
a published **shoulder (withers) height**. The generator did exactly what the row
says — put the joint there and draw the trunk on top of it — so the whole library
stood a half-trunk-depth above the number it was authored from.

---

## 1. The four lines of evidence, before anything was changed

**1. The row's own help text.** `forge/spec.py` offers as real values *"a badger
0.38, a wild boar 0.60, a red fox 0.57, a horse 0.62, a bison 0.64, a moose 0.70,
a giraffe 1.30."* A giraffe's shoulder **joint** is not at 1.3 of its head-body
length. 1.3 is where the top of its withers is. Those are published shoulder
heights.

**2. The biome files quote the same figures in metres.** `docs/biomes/*.md` list
land animals as `head-body / shoulder`, e.g. American bison `2.8 / 1.8 sh`.
Parsed over every row that says "shoulder" out loud — 71 species — the authored
`quad.shoulder_h × quad.length_m` reproduces the documented shoulder height:

| | |
|---|---|
| median of (spec shoulder_h × head-body) ÷ documented shoulder height | **0.996×** |
| within 5% | **54 of 71** |

That is what makes this a definition error rather than a disagreement. **The
numbers were entered correctly, into a slot that means something else.**

**3. The built asset measures a third taller than either.**
`quadprobe.m_withers_h` on the shipped library, variation off, seed 1:

| | median | range | within 10% |
|---|---|---|---|
| measured withers ÷ (spec shoulder_h × head-body), all 131 | **1.354×** | 0.79–2.27 | 2 |
| the same, over the 108 species that **stand** | **1.350×** | 1.21–1.78 | **0 of 108** |
| measured withers ÷ biome-documented shoulder height (n=30, strict parse) | 1.308× | 0.96–1.86 | 1 |

Not one standing species in the library was within 10% of its own authored
height, and every one of them was too tall, never too short.

**4. The reference silhouettes agree, from a source that shares no code with any
of the above.** `tools/reffit.py report`, 80 species: belly clearance 1.26×,
back height 1.22×, trunk depth 1.17×.

---

## 2. The measurement defect that nearly sent this the wrong way

**Every ratio measured against silhouette LENGTH came out high by about the same
amount.** Belly 1.26×, trunk depth 1.17×, back height 1.22×. One offset shared by
three independent numerators is one defect in the denominator they share, not
three defects — and `length` is the whole x-extent of the drawing, which is
exactly what a pose moves. Measured on our own assets:

| | |
|---|---|
| our silhouette length ÷ our own authored head-body length | **0.984** |
| our silhouette length ÷ (head-body + tail) | **0.826** |

Our generator carries a tail at −35° and a neck at +30°, so an asset measures
across at 98% of its own head-body length and 83% of head-body-plus-tail, while a
PhyloPic artist may draw the tail streaming out behind. **So "26% too high" was
partly an artifact of the ruler**, and fitting `belly ÷ length` straight to 1.0
would have been fitting our tail carriage to somebody else's.

`refsil.leg_share` — belly clearance ÷ back height, both measured up from the
same ground line — has no length in it at all. On the shipped library it read
**1.067×** the references, not 1.26×.

**A rejected second attempt, with the numbers, so it is not retried.** The
obvious length-free ruler is fore-foot to hind-foot span. It is useless here:
our assets stand square, so the left and right of a pair project onto each other
and 2 legs are tracked, while the reference corpus is drawn mid-stride and tracks
3 to 5. Our span measured 0.375 of body length against the reference's 0.742 on
`wood-bison`, and **2.2× library-wide** — that is the stride, not the animal.
Same class of error as the horizontal cut across a slanted leg that
`docs/reference-fitting-research.md` §3.2 records.

---

## 3. What was done

`tools/refstance.py`, three commands, all offline, none of them touching
`forge/`.

**`freeze`** captures every species' authored `quad.shoulder_h` into
`refs/withers-target.json` **once**, with the biome-file figure beside it where
one exists. The target cannot be read out of the spec afterwards, because the
row being solved is the row that carries the target — a second run reading its
own output would lower the animal again and print a tidy table every time.
`tools/retune_quad_bulk.py` hit exactly this and its fix was the same: the
baseline is a file.

**`fit`** solves `quad.shoulder_h` down until the **measured** withers lands on
the authored height. Standing species only: a sprawling lizard's limbs leave the
flank, and a kangaroo's withers is not a load-bearing height, so 15 sprawling and
8 bipedal specs are printed as not eligible rather than quietly included.

**`trunk`** then solves `quad.depth` until the **leg share** matches the
reference silhouettes, re-solving the withers at every step so the first pass is
not undone.

### Which knob did the work, and the fight was not where it was expected

The brief expected lowering an animal onto shorter legs to fight the girth gate.
**It does the opposite.** Withers height is the *denominator* of girth/withers,
so lowering the animal raises that ratio: the library went from a median 1.00 —
with 38 of 131 below the 0.95 the previous retune targeted — to a median 1.25,
inside the live 1.14–1.38 range for the first time, with **0 of 131** below the
target. The trunks had been absorbing the height error all along:
`retune_quad_bulk --trunk` deepened them to reach 0.95 of an *inflated* withers,
which is ~1.28 of the true one.

So `quad.shoulder_h` did the work on **85 of the 106** species that moved.

**The real fight is exactly one equation.** Belly clearance is
`withers − trunk depth`, and nothing else. What the trunk hangs below its axis is
subtracted from what it stands above it — which means `quad.belly`, the row that
looks like the obvious knob for a high belly, **cancels out of the answer
entirely and cannot help.** Once the withers is pinned, only a shallower trunk
puts daylight under the animal. That bites the short-legged deep-chested species
and nothing else: 21 of 106 needed `quad.depth` in the stance pass — bear,
badger, hippopotamus, stoat, rock hyrax, red squirrel and the like — and each is
printed with the depth it needed.

### The results

| | before | after |
|---|---|---|
| measured withers ÷ authored, standing species, median | 1.350× | **1.003×** |
| standing species within 10% of their own authored height | **0 of 108** | **106 of 108** |
| `refsil` leg share ÷ reference, median (78 species) | 1.067× | **0.957×** |
| leg share, mean absolute error against the references | 21% | **19%** |
| leg share, species within 10% | 25 of 72 | **37 of 78** |
| belly ÷ silhouette length, median | 1.264× | 0.795× |
| back ÷ silhouette length, median | 1.223× | 0.876× |
| `quadprobe --bulk` failures | 1 (`maned-wolf`) | **0** |
| girth/withers, median over 131 | 1.00 | **1.25** |
| species below the 0.95 retune target | 38 of 131 | **0** |
| `quadprobe` things to look at, all five modes | 1 | **0** |

106 specs changed. 36 trunks changed by the reference pass, 17 of them stopping
short at the girth floor.

---

## 4. Where this is honest about not landing

> **CLOSED 2026-08-15 — see `docs/quadruped-limb-regression.md`.** The diagnosis
> below is right about the mechanism and wrong about the cause. The limb radius
> floor is not what did it: measured across three commits, the drawn diameter
> barely moved (median 3 → 4 voxels) while the **visible** limb halved (median
> 15 → 8 voxels), because a lowered trunk covers more of its own leg. Underneath
> that sat a dead row — `quad.leg_thick` was on its own lower bound in **49 of
> 131 specs** and overridden by the floor in **68** — which no render could show.
> Repaired and re-fitted in both directions, against life: 1.46× → **1.29×**,
> mean absolute error 69% → **56%**, and 1.70× → **1.21×** on the 41 species with
> a fit-quality reference.

**Limb thickness over limb length went the wrong way, and it is reported rather
than hidden.** Against the references it moved from 1.04× to **1.40×**, mean
absolute error 32% → 69%. The mechanism is resolution, not shape: the limb
diameter fell everywhere in absolute terms (`plains-zebra` 7 → 5 voxels across,
`red-deer-stag` 7 → 5, `moose` 5 → 3) while the free limb it is divided by fell
faster, and `forge/quadruped.py` floors a limb radius at one voxel so it cannot
follow all the way down. The direction is the one the owner asked for — he called
the old animals wireframes — and the ratio the previous proportion work chose as
its diagnostic **rose on every species and fell on none**, which is the
constraint that was set. But against life these limbs are now thick, and a future
pass that wants photoreal limbs will have to reopen
`docs/reference-fitting-research.md` §7 rejection 1, which forbids thinning.

**`belly ÷ silhouette length` crossed its target rather than landing on it**,
1.26× → 0.795×. §2 is why that number is not the one being fitted; the length-free
leg share is, and it landed at 0.957×. Both are printed by `reffit report` and
neither is hidden.

**17 species stopped short at the girth floor**, and they are mostly deer:
`red-deer-stag` reached a leg share of 0.449 where its references ask 0.544, and
a trunk slim enough to get there falls below `GIRTH_WANT = 0.95`. That floor is
`tools/quadprobe.py`'s and was set by the previous proportion work.

> **WITHDRAWN 2026-08-15.** This paragraph originally continued: *"a real red
> deer's chest girth is about 0.92 of its withers, i.e. below the library's own
> floor, because the 1.14–1.38 livestock figures behind that floor are cattle,
> horses and goats and not cervids."* **That 0.92 carried no source and the
> literature contradicts it.** Read per file with the units established in each
> case, adult cervids run **1.03 to 1.23** — roe deer 1.026 (n=90), moose 1.085
> (n=23), red deer 1.113 (n=290) and 1.131 (n=76), fallow 1.133, sika 1.155–1.227
> (n=254–322), reindeer 1.215 (n=98) — against 1.10–1.41 for domestic bovids and
> equids. The leanest deer found anywhere is **1.008**, so a 0.95 floor sits 6%
> below it and cannot bind on any deer. The library's own cervids already measure
> 0.95–1.25 against those figures, i.e. at or *below* life in the chest, so
> lowering the floor would move them further from real deer.
> **No per-clade floor was made.** Sources, the four literature traps found on
> the way, and what could not be sourced are in
> `docs/quadruped-limb-regression.md` §7 and §8.

Whatever holds those 17 short, it is not a floor that is 6% clear of every
published deer.

**Two species were held entirely.** `european-hedgehog` cannot be lowered without
its foreleg dropping to 2 voxels across; `striped-skunk`'s belly reaches the
floor at 2 voxels. Both are printed as held, with the floor that stopped them.

**23 species were never eligible** — 15 sprawling, 8 bipedal.

**A caps regression was caused and caught.** `fennec-fox` came out of the first
stance run with its head touching the neck at a corner only, failing
`quadprobe --parts`. The guard written to prevent that had a hole worth
recording: **the solver pins the variation draw off and the probe does not**, so
the pinned build was clean and the failure only existed on the drawn one.
`refstance._joints_ok` now checks the drawn build once on the candidate about to
be written, and `_settle` backs the change off in eighths until it rigs.

---

## 5. The trap this leaves behind, and the one-line fix that is not mine to make

The specs now hold `quad.shoulder_h` values that are **joint heights** — bison
0.348, horse 0.399, red deer 0.401 — and no longer match the biome files they
were read from. The next person to author a species from `docs/biomes/*.md` will
type the published shoulder height into the row again and rebuild the defect.

Two things stand against that:

1. `refs/withers-target.json` keeps the authored intent, so
   `python tools/refstance.py report` prints any species standing away from its
   documented height. That is a regression check and it is the reason the
   targets are frozen in a file rather than recomputed.
2. **`forge/spec.py`'s help text for `quad.shoulder_h` should say so**, and that
   file belongs to another agent. The change wanted is to the opening line and
   the examples:

   > Height of the SHOULDER JOINT above the ground, as a fraction of head-body
   > length — **which is NOT the "shoulder height" a field guide or
   > `docs/biomes/*.md` publishes.** A published shoulder height is the WITHERS,
   > the top of the back over the shoulder, and the joint sits roughly 0.7 of it.
   > Typing a withers figure here stands the animal a half-trunk-depth too tall,
   > which is what all 131 species did until 2026-08-15
   > (`docs/quadruped-stance-height.md`). Solved values, not published ones: a
   > badger 0.26, a wild boar 0.42, a red fox 0.41, a bison 0.35, a horse 0.47,
   > a red deer 0.45, a moose 0.51.
   > Check any new species with `python tools/refstance.py report`.

---

## 6. Reproducing it

```
python tools/refstance.py report          # how far every species stands from its own spec
python tools/refstance.py fit --dry       # what the shoulder solve would move
python tools/refstance.py trunk --dry     # what the reference leg share would move
python tools/reffit.py report             # the silhouettes' verdict on all of it
python tools/quadprobe.py --read --stance --parts --caps --bulk
```

Saved output of the run this file describes is in `out/refstance/` (stance and
probe, before and after) and `out/reffit/` (references and renders).
