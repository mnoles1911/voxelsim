# Birds and fish against measured references: mostly right, and where they are not

Companion to `tools/refshape.py`, `refs/bird-reference.json` and
`refs/fish-reference.json`. Written 2026-08-15, extending
`docs/reference-fitting-research.md` — which fitted land animals to CC0
silhouettes — to the two kinds nothing had ever been fitted to.

**The gap.** `docs/biomes/README.md` §8 says outright that every size in this
library is an unsourced approximation. 127 bird specs and 124 fish and cetacean
specs were authored the same way the land animals were: one agent's judgement
against other shipped specs. This file is a measurement of how far that got.

**The headline, both kinds, and it is not the same answer.**

| | median ours ÷ reference | **mean absolute error** | within 10% |
|---|---|---|---|
| **birds** — span ÷ length | 1.07× | **11%** | 60 of 102 |
| **birds** — wing aspect ratio | 0.96× | **10%** | 50 of 85 |
| **birds** — tail ÷ length | 0.84× | **22%** | 19 of 102 |
| **fish** — length : depth | 1.04× | **23%** | 28 of 97 |
| **fish** — depth : width | 1.04× | **24%** | 23 of 97 |
| **fish** — deepest point | 0.87× | **14%** | 48 of 106 |
| **fish** — dorsal origin | 0.94× | **30%** | 22 of 106 |
| **fish** — operculum | 0.94× | **14%** | 51 of 106 |
| **fish** — peduncle depth | 0.79× | **31%** | 19 of 106 |
| **fish** — caudal fin height | 0.99× | **43%** | 17 of 106 |
| *(for scale)* quadrupeds, limb thickness, before any fitting | 1.04× | 37% | 11 of 80 |

**Every median is inside 1.0 ± 0.21 and three of the mean absolute errors are
over 30%.** That is the same shape the land animals had and the reason both
columns are printed: reporting the median alone would have closed this exercise
with "the library is fine", which is true of the middle and false of the
animals. But the answer here is genuinely different from the land animals'.
**Neither kind has a defect the size of the one the quadrupeds had**, and §5
recommends stopping on almost all of it.

> **A number in this file was wrong first and it is worth recording how.** The
> fish `length : depth` line originally read **46% mean absolute error over 106
> species**, and that was the largest figure in the whole exercise. It was not
> the library. It was **nine depressiform species — four rays and five flatfish
> — being scored against the compressiform `short-deep` class**, which is the
> opposite axis (§3). Corrected, the same measurement is **23% over 97 species**,
> and the biggest apparent defect in either kind disappears into the reference
> table's own error. Nothing downstream could have told those apart: a wrong
> reference produces a large, confident, plausible number.

---

## 1. Why this is a table comparison and not a second silhouette pipeline

`tools/refsil.py` does not transfer. It works by finding a **belly line** — the
lowest row of the silhouette where two separate runs become one — and then
tracking legs upward through a band above it. A fish has no legs, and a PhyloPic
bird is usually drawn **in flight with the wings spread**, which makes the
lateral extent of the drawing the wing rather than the animal. Both would return
a number rather than an error, which is the failure mode this project keeps
paying for.

What does transfer is the **source discipline**, and both kinds already have
measured proportions in this repository, read off real morphometric datasets and
hand-typed precisely because the datasets are not licence-clean:

- **fish** — FishShapes v1 (16,523 specimens) joined to FishBase's own shape
  classification, a 93% join, confirmed independently by FISHMORPH (8,342
  species) to within about 10%; plus **7,452 landmarked specimens** for fin
  positions. `docs/fish-shape-research.md` §1, §2 and §4.
- **birds** — 94 Cornell species accounts joined to AVONET tail lengths, 88
  species joined; plus Alerstam, Rosén, Bäckman, Ericson & Hellgren (2007),
  *"Flight Speeds among Bird Species"*, PLoS Biology 5(8):e197,
  [Protocol S1](https://doi.org/10.1371/journal.pbio.0050197.sd001) — 129 species
  with complete biometry from **33,610 individual measurements**.
  `docs/bird-shape-research.md` §3 and §5.

Those medians are checked in as `refs/*-reference.json` with their sources per
entry. **Nothing here redistributes FishBase, FishShapes, FISHMORPH or AVONET.**
`docs/fish-shape-research.md` §4 refused FishBase (non-commercial) and
FishShapes (its Zenodo and Dryad metadata say CC0 and its own abstract says
CC BY-NC), and `forge/language.py` already carries hand-typed medians on exactly
this basis.

**PhyloPic covers birds and fish and is not used here.** It is the right second
source and it would be an independent one, but it needs a bird-and-fish
equivalent of `refsil.measure` and an orientation rule (which end of a fish
silhouette is the head), and neither should be built before a measurement says
they are needed. §6 says what that instrument has to do.

---

## 2. Birds: close enough to stop

**span ÷ length 1.07× with an 11% mean absolute error, aspect ratio 0.96× with
10%.** For comparison the land animals' limb ratio was 37% and their belly
clearance 33%. **The bird specs are the best-authored proportions in this
library and they should be left alone.**

That is not a soft verdict, it is a statement about resolution. The bird
reference is a **group median over 19 groups**, several with n = 1 to 3 — the
kingfisher row is one bird and the osprey row is one bird. A median over n = 2
has no business resolving 10%, so a library sitting 10% from it is sitting
inside the reference's own uncertainty. Moving specs at that scale would be
fitting noise.

`tools/refshape.py birds fit` exists and is **not run**. Its rule is a 40%
tolerance around the group median precisely so that a genuinely mis-authored
bird would move and ordinary within-group spread would not. Dry-run, **exactly
one species of 109 is outside it**: `hummingbird`, whose `bird.wing_span` of 2.20
is 1.85× a hummingbird group median of **1.19 computed from two birds**. One
spec against a median of two is not a measurement, and a hummingbird's wings are
the one thing about it anybody would notice, so it is left and printed rather
than moved. If it is ever revisited, revisit the reference first.

**One signal is real and it is not span.** `tail ÷ length` measures **0.84×,
one-directional**, with only 19 of 102 inside 10%: the library's tails are
systematically about 16% short for their group. Unlike the other two ratios this
is a bias and not a scatter, which is what makes it worth believing.

**It is not fitted, and the reason is a shared denominator again.**
`forge/bird.py` normalises five shares — bill, head, neck, body, tail — against
their own sum before anything is drawn. Writing a bigger `bird.tail_frac`
therefore *shrinks the bill, head, neck and body of the same bird*, and the
change would land somewhere nobody asked for it. Closing that 16% honestly needs
a solver against the built asset (`birdprobe.m_tail_run` over `m_length`), not a
spec-row edit. That is the next piece of bird work and it is one number.

**A denominator caveat carried forward from the source, because it will bite
somebody.** "Total length" in the Cornell accounts is bill-tip to tail-tip, so a
long tail inflates the denominator and *crushes* span ÷ length. A magpie measures
1.11 — the lowest in the whole dataset — purely because its tail is half its
total length; on body length alone it is about 2.2. `bird.wing_span` is authored
against total length too, so the comparison above is like with like, **and these
numbers must never be mixed with a span-over-body-length figure from anywhere
else.**

**18 of 127 birds have no group and are reported as unmeasured**, not fitted
against the nearest-looking row: `common-cuckoo`, `common-ostrich`,
`crowned-sandgrouse`, `emu`, `eurasian-hoopoe`, `great-blue-turaco`,
`great-bustard`, `great-hornbill`, `greater-rhea`, `greater-roadrunner`,
`hoatzin`, `houbara-bustard`, `lilac-breasted-roller`,
`magnificent-frigatebird`, `scarlet-macaw`, `secretary-bird`, `sunbittern`,
`toco-toucan`. Three of those do not fly at all. A 19-group table built from 88
north-temperate species has no line for any of them, and guessing one is the
error `refs/species-latin.json` exists to prevent.

---

## 3. Fish: one ratio is genuinely wrong and the rest are inside the data

**`length : depth` is the ratio the sources say separates one fish from another**
— it spans a factor of eight across the four FishBase shape classes, where a
bird's whole wingspan design space is under threefold. Measured over the 97
species that have a class: median **1.04×**, **mean absolute error 23%**, 28
within 10%, range 0.46× to 1.79×.

**And only 11 of those 97 are outside their class's published p10–p90 band**,
four of them by more than 20%. Those two facts together are the finding. The
bands are wide because real fish are — fusiform runs 2.4 to 4.7 — so a 23% mean
error against a class *median* is mostly the library correctly spreading species
across a class, and the mean error is measuring distance from the middle of a
distribution nobody should sit in the middle of.

**So the fish are close too, and the honest verdict is close to "stop".** The
fit below moves the mean absolute error from 23% to **22%** and pulls the worst
species in from 1.79× to 1.67×. That is a small return, and it is reported as a
small return rather than dressed up: the value this pass delivered on fish is the
**measurement** and the **depressiform hole**, not the four numbers it wrote.

So the fit is deliberately narrow, and it fits **to the edge of the band, not to
the median**. Collapsing every species onto its class median would rebuild the
exact defect `docs/reference-fitting-research.md` §4 found in `quad.leg_thick`:
*"the defect is not the value of the constant; it is that it is a constant."*

`BAND_TOL = 1.20` then holds back the species that are merely in the tail of
their own class. **`ocean-sunfish` is why.** It measures a length:depth of 1.3
against a short-deep p10 of 1.5, and a *Mola mola* really is as deep as it is
long — pulling it to the p10 would have made the most distinctive silhouette in
the library more ordinary on the authority of a percentile.

### What moved

| species | class | `fish.depth_ratio` | length:depth was | class band |
|---|---|---|---|---|
| african-lungfish | eel-like | 0.120 → **0.097** | 8.3 | 10.3–34.9 |
| brook-lamprey | eel-like | 0.130 → **0.097** | 7.7 | 10.3–34.9 |
| giant-moray | eel-like | 0.130 → **0.097** | 7.7 | 10.3–34.9 |
| great-white-shark | fusiform | 0.175 → **0.213** | 5.7 | 2.4–4.7 |

Four species, and the whole library-wide effect of them is 23% → 22% mean
absolute error with the worst species coming in from 1.79× to 1.67×.
`tools/fishprobe.py --read` reports the same four pre-existing
flags before and after (`common-dace` and `european-anchovy` THIN,
`freshwater-stingray` and `thornback-ray` SHALLOW) and no new ones.

### Two things the dry run caught by being looked at

**A manta ray was about to be turned into a disc stood on its edge.** Nine
species here are **depressiform** — flat top-to-bottom and wide across: four
rays and five flatfish. FishBase's four-class table **has no depressiform row**,
and they were first hand-assigned to `short-deep` because that is the class whose
length:depth number they resemble. That is exactly backwards: short-deep means
*compressiform*, deep and narrow, the opposite axis. The fit proposed
`giant-manta-ray` 0.100 → 0.160, `turbot` 0.220 → 0.333, `european-plaice`
0.160 → 0.256. They now carry a `depressiform` class with every number null, and
`refshape` reports them and never fits them. **FishShapes v1 is the one source
that carries body width and would settle them, and it is the one this project
refused on licence.**

**Three hand assignments were wrong and the fit is what showed it.**
`wels-catfish`, `stone-loach` and `small-spotted-catshark` were assigned
`eel-like`, which asks for a length:depth of at least 10.3 — that would have
taken a wels catfish, a fish with a broad flat head and an eel-like tail, to a
tenth of its length in depth. All three are `elongated`, all three sit inside
that band as authored, and all three now move nothing.

### What is reported and NOT fitted, with the reason for each

- **`fish.depth_at`** (deepest point, 0.87× / 14% MAE) — **because it cannot be
  verified.** `fishprobe.m_deepest_at` is the argmax of the whole silhouette's
  column depth, so on any fish with a dorsal or anal fin **the fin is the deepest
  column** and the body underneath it is invisible to that measurement: swept
  from 0.14 to 0.50 on a brown trout it reports 0.382 every time. The row *is*
  read — `m_depth` moves a bream from 24 to 30 voxels over the same sweep — but a
  fit here could write a number that nothing checks, which is this project's
  signature failure. **A fin-free deepest-point measurement is wanted in
  `fishprobe` and does not exist.**
- **`fish.head_frac`** (operculum, 0.94× / 14%) — the closest ratio in the set
  and inside the landmark band for half the library.
- **`fish.dorsal_start`** (0.94× / 30%) — the landmark band is 0.235–0.585,
  which is most of a fish; almost nothing is outside it.
- **`fish.peduncle`** (0.79× / 31%) — a real one-directional signal, the same
  shape as the birds' tails: our caudal peduncles are systematically about a
  fifth slimmer than the all-teleost 0.34. It is left because the peduncle is
  also what the caudal fin flares out of (`forge/fish.py:_caudal` starts its
  flare at `wrist`), so moving it moves the tail too, and that coupling has not
  been measured.
- **`fish.width_ratio`** (1.04× / 24%) — inside the class figure on median, and
  the nine species where width is the whole story are the nine with no reference.
- **caudal fin height** (0.98× / 42%) — it is *derived*
  (`fish.caudal_span × fish.depth_ratio`), so it is not a row, and fitting it
  means moving one of two rows that both already have references of their own.

**18 cetaceans are not measured at all.** They are mammals; not one number in
`refs/fish-reference.json` applies to them. `docs/marine-megafauna-research.md`
is where their proportions would have to come from and this pass did not go
there.

---

## 4. Every row that was fitted was proved to be read first

`refshape <kind> honoured` sweeps each candidate row to 0.7× and 1.4× of itself
and measures the built asset. This is the check `reffit dead` exists for: on the
same day, `quad.leg_thick` was found sitting on its own lower bound in 49 specs,
drawing identical animals, because a floor in `forge/quadruped.py` was doing the
work.

```
row                          x0.7     x1.0     x1.4   verdict
fish.depth_ratio             4.50     6.50     9.50   moves
fish.depth_at               12.00    13.00    13.00   moves
fish.dorsal_start            0.32     0.41     0.50   moves
fish.head_frac               5.00     4.00     4.00   moves
fish.peduncle                1.00     1.00     2.00   moves
fish.caudal_span             6.00     8.00    12.00   moves
bird.wing_span              27.00    37.00    53.00   moves
bird.wing_aspect            11.00     8.00     6.00   moves
bird.tail_frac               5.00     6.00     7.00   moves
```

**The sweep itself had the bug it was written to catch.** Its first version used
a single one-voxel floor for "did this move", which is right for a voxel count
and absurd for a fraction of body length: `fish.dorsal_start` moved 0.32 → 0.50
across its sweep — the row working perfectly — and was reported **DEAD**. Each
pair now carries its own floor.

---

## 5. Recommendations

1. **Stop on bird span and bird aspect ratio.** 10–11% mean absolute error
   against a group median with n as low as 1 is inside the reference's own
   resolution. There is nothing here to fit.
2. **Do the bird tail properly, as a solver.** 0.84× one-directional over 102
   species is the only real bird signal, and it cannot be reached by editing
   `bird.tail_frac` because five shares are normalised against their sum.
3. **Stop on the fish landmarks, and very nearly on fish depth too.** Operculum,
   dorsal origin, deepest point and width are all inside their published bands
   for most of the library, and `length : depth` — the one ratio that looked like
   a real defect — is 23% once the reference table is right, against 37% for the
   land animals before any of that work started. **A second fish pass is not
   worth running until there is a new source**, not a new solver.
4. **Build a fin-free deepest-point measurement in `fishprobe`** before anybody
   fits `fish.depth_at`.
5. **Nine depressiform species have no reference at all** and will not until
   somebody resolves FishShapes' licence contradiction with its authors, or finds
   a width source that is clean.
6. **PhyloPic next, if a second source is wanted** — §6.

---

## 6. What a bird-and-fish `refsil` would have to do, if it is built

Recorded so the next pass does not rediscover it:

- **A fish silhouette has no orientation cue as strong as a belly line.** The
  usable one is topological rather than a threshold, in the same spirit as
  `refsil`'s belly: the **caudal peduncle** is a local minimum in column depth
  inside the outer quarter, followed by the tail fin flaring back out. The nose
  end has no such minimum. Do not use "the deepest point is forward of centre" —
  that is one of the landmarks being measured and using it to orient the picture
  makes the measurement circular.
- **A bird silhouette must reject spread wings before it measures anything.**
  A flying PhyloPic bird's x-extent is a wing, not a bird. The cheap test is that
  a perched bird's silhouette has one tall mass and a flying one has a lateral
  span several times its body depth — `birdprobe` already measures the perched-
  to-flying span ratio at **×4.51**.
- **`refsil.from_grid`, `_crop`, `_runs` and `overlay` are all kind-agnostic**
  and can be reused as they are. Only `measure`, `usable` and `_track` assume
  four legs.
- **Draw the overlay before trusting any number.**
  `docs/reference-fitting-research.md` §3.2 records four defects that a table of
  ratios could not show and a line drawn on the picture showed instantly.

---

## 7. What could not be sourced, and where this is still estimating

**Could not be sourced.**

- **Any body-width source for rays and flatfish** that is licence-clean.
  FishShapes v1 is the only dataset found anywhere with body width and its own
  terms contradict each other.
- **Per-group percentiles for birds.** The bird table publishes a group median
  and a group range and no p10/p90, which is why the bird fit uses a 40% factor
  where the fish fit uses a published band. A range over n = 2 is not a
  distribution.
- **Anything at all for cetaceans** in this pass.
- **A trophic niche per bird species**, which is what the AVONET bill table
  (`docs/bird-shape-research.md` §6, n = 11,009, granivore 0.537 to nectarivore
  0.100) is indexed by. The table is good and the assignment was not made, so
  bills are not measured here.

**Where this is estimating.**

- **The group and class assignments are hand judgements.** 106 fish into four
  FishBase classes and 109 birds into 19 groups, by one person reading names.
  Three were caught wrong by the fit itself (§3) and there is no reason to think
  three is all of them. They are checked in as data, one line per species, so a
  disagreement is an edit and not an argument.
- **`BAND_TOL = 1.20` and the birds' 40% tolerance** are bounds, not
  measurements, exactly as `reffit.MAX_LIFT` is.
- **`MIN_MOVE = 0.03`** exists because every spec write changes that species'
  seed salt and therefore every individual of it. A 0.7% move to chase a band
  edge changes every fish of that species for a change nobody can see.
- **The bird span and tail figures are against TOTAL length**, bill tip to tail
  tip, with the magpie deflation described in §2 baked into both sides.
