# The limbs did not get thicker. The visible leg got shorter.

Companion to `tools/reffit.py` (`dead`, `fit --thin`), `refs/limb-target.json` and
`tools/quadprobe.py`. Written 2026-08-15, closing the regression
`docs/quadruped-stance-height.md` §4 reported against itself:

> *"Limb thickness over limb length went the wrong way... Against the references
> it moved from 1.04× to 1.40×, mean absolute error 32% → 69%."*

Measured again at the start of this pass over the 78 species with both a spec and
a reference: **median 1.46× life, mean absolute error 69%, 11 of 78 within 10%.**

---

## 1. What actually happened, in one table

`quadprobe.m_limb_dia` and `m_limb_len` on the whole library, variation pinned
off, seed 1, at three commits — each built with **its own** code and specs:

| | 040fb58 (before the thickness pass) | 17cc742 (the state the owner calls 0.250) | now, before this pass |
|---|---|---|---|
| median thickness ÷ length | 0.231 | **0.250** | **0.500** |
| median foreleg, voxels across | 3.0 | 4.0 | 4.0 |
| median FREE limb, voxels long | 15.0 | **15.0** | **8.0** |

**The numerator did not move and the denominator halved.** Absolute diameters
actually *fell* on 38 of 131 species — a zebra went 7 voxels across to 5 — while
the visible limb, which is everything below the belly, went from 15 voxels to 8.
The ratio doubled because the animal came down onto its true withers and its
trunk now covers more of its own leg.

That is the third time this row has been divided by the wrong length, and the
first two are already written down: `quad.leg_thick` was a fraction of the
**trunk's depth** until 90f512c, and `quad.shoulder_h` was read as the **withers**
until 56a91e4. This one is subtler because both lengths are real: the generator
divides by the limb **joint to ground**, and `quadprobe`, `refsil` and the eye all
divide by the limb **below the belly**. Those two agreed to about 1.2× while the
library stood too tall. They now differ by 2 to 3× on any deep-bodied species —
more than half a muskox's leg is inside the muskox.

## 2. And underneath it, a dead row on 68 of 131 species

`forge/quadruped.py` computes

    leg_r = max(1.0, 0.5 * quad.leg_thick * limb_v)

so wherever `0.5 * leg_thick * limb_v` is under one voxel **the row does
nothing**: the floor draws the limb, three voxels across, whatever the spec
says. Measured, that was **68 of 131 species**, and **49 of them carried the
row's own lower bound, 0.05**.

0.05 is not an authored value. It is what the 90f512c conversion clamped to when
its arithmetic went out of range — `red-fox` went `0.176 → 0.05` in that commit —
and the floor then hid the clamp perfectly. Every one of those 49 species renders
exactly as it would with any other value, so **no render, no gate and no
parameter sweep could ever have shown it.** It is this project's signature
failure, and it shipped inside the commit that was meant to end it.

The two populations behave completely differently under a lattice change, which
is how they were separated. Rebuilt at 5, 2 and 1 cm:

| | thickness ÷ length at 5 cm | 2 cm | 1 cm |
|---|---|---|---|
| `reindeer` (row dead at 0.05) | 0.429 | 0.167 | **0.079** |
| `alpine-ibex` (dead) | 0.500 | 0.176 | **0.086** |
| `muskox` (row live at 0.635) | 1.875 | 1.417 | **1.192** |
| `lion` (live at 0.176) | 0.429 | 0.238 | **0.220** |

Where the row is dead the ratio is a pure lattice artefact — a three-voxel
minimum divided by however many voxels the leg happens to be. Where the row is
live the ratio is nearly lattice-invariant, which is what a dimensionless
proportion should be, **and the value it is invariant at is wrong**: 0.635 means
a limb two thirds as thick as it is long. Eleven specs sat between 0.30 and 0.69.

So `quad.leg_thick` was not carrying anatomy on most of the library. It was
carrying the lattice: solvers chasing `LIMB_MIN_VOX = 3` need `leg_thick ≥ 0.3`
on a 10-voxel limb at 5 cm and `≥ 0.06` on a 50-voxel limb at 1 cm, and the row
absorbed the difference at both ends.

## 3. What was NOT the fix, with the numbers

**Raising the belly by slimming the trunk.** `docs/quadruped-stance-height.md` §3
is right that belly clearance is `withers − trunk depth` and nothing else, so a
shallower trunk is the only thing that puts daylight under a pinned animal. It
was measured before being adopted and it does not pay:

| `reindeer`, `quad.depth` × | 1.00 | 0.85 | 0.70 | 0.55 |
|---|---|---|---|---|
| free limb, voxels | 7 | 7 | 8 | 9 |
| thickness ÷ length | 0.429 | 0.429 | 0.375 | 0.333 |
| withers, voxels | **22** | 20 | 19 | **17** |
| girth ÷ withers | 1.25 | 1.12 | 1.03 | 0.84 |

A 45% cut in trunk depth buys 22% off the ratio, drops the withers by five voxels
— **which is the stance fix coming undone** — and takes girth under the gate. The
trunk is inside the withers as well as under it, so this lever moves the thing
that was just solved.

**A floor on the drawn DIAMETER.** The obvious way to honour "thickness/length
must not fall" is to refuse to draw any limb thinner than the accepted library
did. Measured, **38 of 131 species are already below the 17cc742 diameter**, so
that floor refuses every move including the ones that fix the regression. The
floor has to be the **ratio**, where 0 of 131 are below. The two readings of one
sentence differ by the whole exercise.

**Fitting straight to the silhouettes.** Allowed to run to the reference with only
`LIMB_MIN_VOX` under it, the fit takes twelve species to three voxels across —
`brown-bear` 7 → 3, `wood-bison` 5 → 3, `warthog` 5 → 3. That is the wireframe
library the owner rejected, rebuilt on the authority of a drawing.
`docs/reference-fitting-research.md` §7 rejection 1 is right about that.

## 4. What was done

**`reffit dead --write`** finds every spec whose `quad.leg_thick` the generator
cannot honour and rewrites it to `2.0 / limb_v` — the ratio the floor is
**already drawing**. 68 specs, and the value is floored to three decimals rather
than rounded so that the radius cannot land a thousandth above 1.0 and let the
rasteriser add a corner voxel. Verified: **0 of 393 builds changed voxel count**
(131 species × 3 seeds, variation pinned off).

Fourteen of the 68 had to be **backed off below** the honest value, and the
reason is a hole this repository has already documented once
(`docs/quadruped-stance-height.md` §4: *"the solver pins the variation draw off
and the probe does not"*). `2.0 / limb_v` sits exactly on the floor at the pinned
size; an individual drawn a few per cent larger crosses it. That lifted
`lesser-egyptian-jerboa`'s hind feet two voxels off the ground and failed
`quadprobe --stance` on a change that was provably a no-op at seed 1 with
variation off. `_settle_dead` now walks the value down in eighths until the
**varied** draw measures what it did before.

**`reffit fit --thin`** lets the reference thin a species as well as thicken it.
The prohibition is replaced by the owner's own gates rather than lifted:

- the drawn foreleg may never go under `quadprobe.LIMB_MIN_VOX` (3 voxels);
- `girth ÷ withers` may never go under `GIRTH_MIN` (0.90);
- and **thickness ÷ length may never fall below the highest of what the library
  measured at 040fb58, 90f512c and 17cc742**, frozen per species in
  `refs/limb-target.json`.

That last one is the constraint in the owner's own words — *"thickness/length
must not fall on any species; its median is 0.250"* — turned into a file. 17cc742
is the commit whose median **is** 0.250, confirmed by rebuilding it. The baseline
is frozen rather than recomputed for the reason `refs/withers-target.json` is:
a solver that reads its own output as its own floor thins the library a little
further every run and prints a clean table each time.

19 species moved. 15 are held and printed with the gate that held them.

## 5. What it bought

Reference and asset measured by the same code (`tools/refsil.py`), variation off,
seed 1, 78 species with both a spec and a reference:

| limb thickness ÷ limb length, against life | before | after |
|---|---|---|
| median | 1.46× | **1.29×** |
| **mean absolute error** | **69%** | **56%** |
| within 10% of its own reference | 11 of 78 | **15 of 78** |
| the 41 fit-quality species: median | 1.70× | **1.21×** |
| the 41 fit-quality species: mean absolute error | 69% | **45%** |
| species that got worse | — | **0** |

| | before | after |
|---|---|---|
| specs on the row's lower bound 0.05 | 49 | **5** |
| specs whose `quad.leg_thick` the floor overrides | 68 | 72 |
| species below any historic thickness ÷ length baseline | 0 | **0** |
| thinnest foreleg drawn anywhere | 3 voxels | **3 voxels** |
| species failing a `--bulk` gate | 0 of 131 | **0 of 131** |
| species below the 0.95 girth *target* (not a gate) | 0 of 131 | **1 of 131** |
| `quadprobe --read --stance --parts --caps --bulk` | 0 to look at | **0 to look at** |
| the same at 6 seeds | — | **0 to look at** |
| `forge.cli selftest` / `buildcheck` | PASS / PASS | **PASS 824 / PASS 828** |
| `buildcheck --kind quadruped --seeds 2 3 4` | — | **PASS, 393 builds** |

The one species that crossed below the 0.95 retune *target* is a thinner limb
standing its animal fractionally lower; it is not a gate failure, it is printed
by the probe's own line, and §7 is why 0.95 stays where it is.

Per species, worst first:

| species | life | before | after | before ÷ life | after ÷ life |
|---|---|---|---|---|---|
| muskox | 0.263 | 1.023 | 0.540 | 3.89× | 2.06× |
| przewalskis-horse | 0.128 | 0.329 | 0.196 | 2.56× | 1.53× |
| roe-deer | 0.083 | 0.200 | 0.178 | 2.40× | 2.14× |
| lowland-tapir | 0.220 | 0.489 | 0.348 | 2.22× | 1.58× |
| red-deer-stag | 0.120 | 0.240 | 0.152 | 2.01× | 1.27× |
| wood-bison | 0.222 | 0.445 | 0.269 | 2.01× | 1.21× |
| brown-bear | 0.428 | 0.854 | 0.617 | 2.00× | 1.44× |
| elk-wapiti | 0.123 | 0.239 | 0.205 | 1.94× | 1.66× |
| coyote | 0.167 | 0.307 | 0.187 | 1.84× | 1.12× |
| striped-hyena | 0.178 | 0.315 | 0.179 | 1.77× | **1.01×** |
| moose | 0.155 | 0.273 | 0.207 | 1.76× | 1.33× |
| warthog | 0.198 | 0.338 | 0.203 | 1.70× | **1.03×** |
| blue-wildebeest | 0.113 | 0.175 | 0.125 | 1.55× | 1.11× |
| wild-boar | 0.200 | 0.248 | 0.199 | 1.24× | **1.00×** |
| maned-wolf | 0.152 | 0.183 | 0.167 | 1.21× | 1.10× |
| spotted-hyena | 0.209 | 0.250 | 0.248 | 1.19× | 1.19× |
| plains-zebra | 0.138 | 0.153 | 0.139 | 1.11× | **1.01×** |
| african-wild-dog | 0.170 | 0.184 | 0.158 | 1.08× | 0.93× |

Renders in `out/reffit/sheets/` — before, after and the reference at one locked
scale and one camera, including `plains-zebra`, `grey-wolf`, `red-fox`,
`american-bison`, `greater-kudu` and `pronghorn`, which nobody has complained
about and which are there so that a regression is visible.

## 6. The half that a spec value cannot reach, stated plainly

**71 of 131 species cannot reach any historic thickness ratio at three voxels**,
because their visible limb is 3 to 9 voxels long. 46 species have a free limb of
**four voxels or fewer**. The median smallest achievable ratio, `3 / free limb`,
is **0.375** — so the library-wide `--bulk` median of 0.500 cannot come back to
0.250 by any setting of any row. It is a lattice number.

And the lattice is authored inconsistently across animals of the same size:
`reindeer` and `alpine-ibex` are at 5 cm while `red-deer-stag` and `chamois` are
at 2 cm. Rebuilt at 2 cm, `reindeer` measures 0.145 against a reference 0.107 —
inside 40% instead of 3.7× out — for 29,478 voxels against 2,324.

**That is an owner decision and this pass does not take it.** Cost, not
correctness, is what a lattice tier is about, and `docs/fish-shape-research.md`
§9 records the same call being escalated rather than made. It is the largest
single remaining term in limb thickness against life, and it is the next thing to
look at.

## 7. The girth floor: the measurement, and the answer is no

`docs/quadruped-stance-height.md` §4 says, of the 17 species that stopped at
`GIRTH_WANT`:

> *"a real red deer's chest girth is about 0.92 of its withers, i.e. below the
> library's own floor"*

**That number carries no source and the literature contradicts it.** Chest girth
÷ withers height, read per file with the units established in each case:

| species | girth ÷ withers | n | source |
|---|---|---|---|
| *Capreolus capreolus* (roe), ♂ >24 mo | **1.026** | 90 | [Ann Anat 260:152401](https://backoffice.biblio.ugent.be/download/01K6AACZ0P89ZPN5CERKBH91B1/01K6AAK0PTQ1D6ZA4VAJJ6MWMT), Table 1; units from the caption *"Values expressed in centimetres"* |
| *C. capreolus*, ♀ >24 mo | 1.047 | 94 | same |
| *Alces alces gigas* (moose), ♀ | **1.085** | 23 | [Mamm. Species 154](https://doi.org/10.2307/3503876); units quoted inline: *"Means of measurements (mm) … chest girth 2013 … height at shoulder 1855"* |
| *Cervus elaphus* (red), ♀ ≥2 yr | **1.113** | 290 | [Ital J Anim Sci 15:461](https://flore.unifi.it/retrieve/e398c37a-fa0b-179a-e053-3705fe0a4cff/Body%20measurements%20from%20selective%20hunting%20biometric%20features%20of%20red%20deer%20Cervus%20elaphus%20from%20Northern%20Apennine%20Italy.pdf), Table 2; row labels carry `HS, cm` and `CC, cm` |
| *C. elaphus*, ♂ ≥5 yr | **1.131** | 76 | same |
| *Dama dama* (fallow), ♂ | 1.133 | 8 | [J Food Sci Technol](https://doi.org/10.1007/s13197-020-04352-2), Table 1 |
| *Cervus nippon* (sika), ♂ 2/3/≥4 yr | 1.155 / 1.175 / **1.227** | 254/189/322 | [Animals 16:1325](https://doi.org/10.3390/ani16091325), Table 1; `BH (cm)` defined as *"vertical distance from the highest point of the withers to the ground"* |
| *Rangifer tarandus* (reindeer), ♀ | **1.215** | 98 | [Usp Sovrem Biol 143:454](https://doi.org/10.31857/S0042132423050095); the paper prints the ratio itself as *индекс массивности* = 121.5 |
| horses (three breeds) | 1.136 – 1.156 | 183 / 150 / 6,033 | [Animals 16:1597](https://doi.org/10.3390/ani16111597), [PMC7222724](https://pmc.ncbi.nlm.nih.gov/articles/PMC7222724/), [PMC7277675](https://pmc.ncbi.nlm.nih.gov/articles/PMC7277675/) |
| goats | 1.104 – 1.259 | — / 511 / 318 | [PMC11616873](https://pmc.ncbi.nlm.nih.gov/articles/PMC11616873/), [PMC10294932](https://pmc.ncbi.nlm.nih.gov/articles/PMC10294932/) |
| cattle | 1.233 – **1.407** | 60–600 / 151 | [PMC9421314](https://pmc.ncbi.nlm.nih.gov/articles/PMC9421314/), [PMC10522019](https://pmc.ncbi.nlm.nih.gov/articles/PMC10522019/) |
| sheep, Kazakh fat-tailed | 1.368 | 295 | [PMC12469880](https://pmc.ncbi.nlm.nih.gov/articles/PMC12469880/) |

Girth is defined the same way in every one of these — *"just behind the front
legs"*, *"posterior to the scapula"*, *"just behind the forelegs"* — so they are
comparable.

**The cervid signal is real and it is nowhere near 0.95.** Adult cervids run
**1.03 to 1.23**, median ≈ 1.13; domestic bovids and equids run 1.10 to 1.41,
median ≈ 1.25. The clade difference is about 0.12 and the distributions overlap
heavily. **The leanest deer ever measured in this search is 1.008.** A 0.95 floor
sits 6% below that and cannot bind on any deer.

And the library is already at life. Measured now, `quadprobe --bulk`:

| | ours | life |
|---|---|---|
| red-deer-stag | 1.03 | 1.13 |
| red-deer-hind | 1.07 | 1.11 |
| roe-deer | 1.06 | 1.03–1.05 |
| fallow-deer | 0.99 | 1.13 |
| sika-deer | 1.14 | 1.14–1.23 |
| reindeer | 1.25 | 1.22 |
| moose | 0.96 | 1.09 |
| przewalskis-horse | 0.95 | 1.14–1.16 |

**Our deer are at or below life in the chest already.** Lowering the floor to let
them get slimmer would move them further from real deer, not closer.

**Decision: no per-clade girth floor. `GIRTH_WANT` and `GIRTH_MIN` stay where
they are, and §4's 0.92 is withdrawn as unsourced.** What the data does support,
and what is not in the library anywhere, is that the ratio is an *age and
condition* axis as much as a clade one — sika go 1.14 → 1.23 from two years to
four, red deer 1.05 as a calf to 1.13 as an adult stag — and that the "1.14–1.38
livestock" range quoted in `quadprobe.py` is too narrow at both ends: Sussex
cattle reach 1.41 and Nguni goats fall to 1.10.

**Four traps found in this literature and not walked into**, recorded so nobody
walks into them later:

1. The sambar paper's *"thoracic diameter"* 100.8 cm against a withers height of
   89.6 cm is a mislabelled circumference — the same trap
   `docs/reference-fitting-research.md` §2.8 already flagged on the same paper.
   No number is taken from it.
2. **Two sources were dropped as internally impossible rather than
   reinterpreted**: an Indonesian beef-cattle table with girth 56–68 cm against a
   withers height of 113–133 cm, and four Nigerian cattle breeds at 0.77.
3. Two sub-1.0 figures — dorcas gazelle 0.93, bohor reedbuck 1.00 — are **not**
   withers ratios. That paper's height trait is *"chest height, measured
   vertically from the chest to the ground"*, a different measurement. They are
   the only values anywhere below 1.0 and they are a definitional artefact.
4. **"Girth ≈ π × depth" is not a valid consistency filter for ungulates.** Where
   both are published, sika measure 2.54 and giraffe 1.92: an ungulate thorax is
   laterally compressed. Applying π would have rejected good data.

## 8. What could not be sourced

- **Chest girth AND withers height for white-tailed deer, wapiti, mule deer, or
  any of bison, muskox, ibex, chamois, bighorn or European bison.** Not one of
  the six wild bovids in this library has both measurements published anywhere
  reachable. Muskox has no published chest girth at all. Our figures for those
  species are compared against nothing.
- **Adult male moose withers height** — girth is published, height is not.
- **Farmed red-deer live-weight regressions** (New Zealand, Poland, Czech,
  Slovak, Turkey, Iran), which are the obvious source and are all closed access.
- **Draft and pony horse breeds.** Every horse figure above is a light riding
  breed.

## 9. Where this is still estimating

- **`MAX_THIN = 2.0`** is a bound, not a measurement, exactly as `MAX_LIFT = 1.5`
  is. No pass may more than halve a species' measured ratio.
- **`2.0 / limb_v` as the honest value for a dead row** records what the floor
  draws. It is not a fit and it is not anatomy; it is a spec that stops lying.
  Where those species have a reference, `fit` should do the real work, and on 42
  of them it now can.
- **`refs/limb-target.json` is a floor, not a target.** Nothing in it says the
  17cc742 library was right; it says that is where the owner drew the line.
- **The 5 specs still on 0.05** are ones `_settle_dead` could not raise without
  moving the varied draw. They are still dead rows and they are still counted as
  such by `reffit dead`.
