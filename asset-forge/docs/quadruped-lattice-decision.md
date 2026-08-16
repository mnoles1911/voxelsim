# The land library disagrees with itself about voxel size

**This is an owner decision and it is the last thing standing between the land
animals and the shape they were fitted for.** `docs/quadruped-limb-regression.md`
closed the `quad.leg_thick` regression and had to end by reporting that **71 of
131 species are lattice-limited, 46 of them under five voxels of visible limb**,
and that no value of any spec row can fix that. This file is the measurement
behind that sentence, and the options.

Tool: `tools/quadlattice_ab.py`. Sheet: `out/quad-lattice-ab.png`, one scale of
150 px/m across every cell.

---

## 1. Nothing chose the current assignment

`resolution_cm` is authored per species. Measured 2026-08-15 across 131
quadrupeds: **42 on 1 cm, 63 on 2 cm, 26 on 5 cm.** The 26 on 5 cm are the
*largest* animals in the library — the ones a player sees first, from furthest
away, for longest.

Same-length animals landed on different lattices, and the pairs are absurd read
aloud:

| | on 5 cm | | on 2 cm |
|---|---|---|---|
| lion, 2.0 m | **40 voxels** long | bengal tiger, 2.0 m | **100 voxels** |
| Przewalski's horse, 2.1 m | **42 voxels** | plains zebra, 2.3 m | **115 voxels** |
| muskox, 2.2 m | **44 voxels** | blue wildebeest, 2.2 m | **110 voxels** |
| reindeer, 1.9 m | **38 voxels** | white-tailed deer, 1.8 m | **90 voxels** |
| elk/wapiti, 2.4 m | **48 voxels** | red deer stag, 2.0 m | **100 voxels** |

That is not a design. It is where three separate seeding passes happened to
land. Below roughly 60 voxels along the body there is no `quad.leg_thick` that
can draw a leg: the limb radius floor makes it three voxels wide and nothing
finer is available, which is exactly the dead row that
`docs/quadruped-limb-regression.md` §2 found on 68 species.

The library median is **45 voxels along the body**, and **86 of 131 are under
60**.

## 2. What the move costs, measured

Solid voxel counts, seed 1, `variation.amount` pinned to 0.

**What the library already pays on 2 cm today:** bengal-tiger 27,434 ·
blue-wildebeest 33,022 · plains-zebra 38,001 · greater-kudu 50,339. Call it
**27k–50k**.

**The 15 species at or under 2.4 m, if moved to 2 cm** (a first version of this
file said 20 here and six below; both were the row counts of the sample tables,
not the group sizes — the split is 15 and 11):

| | 5 cm now | at 2 cm |
|---|---|---|
| przewalskis-horse | 1,810 | 26,594 |
| brown-bear | 2,189 | 31,734 |
| reindeer | 2,324 | 32,222 |
| lion | 2,492 | 37,833 |
| muskox | 2,805 | 41,218 |
| elk-wapiti | 3,767 | 55,527 |

**They land inside the band 63 species already occupy.** Moving them is not a
new cost class; it is parity with the animals standing next to them.

**The 11 over 2.4 m are a different question:**

| | 5 cm now | at 2.5 cm | at 2 cm |
|---|---|---|---|
| moose, 2.7 m | 5,433 | **39,550** | 76,783 |
| american-bison, 2.8 m | 6,748 | **53,467** | 105,912 |
| wild-yak, 3.0 m | 12,651 | 100,530 | 198,731 |
| white-rhinoceros, 3.8 m | 18,308 | 141,167 | 279,113 |
| hippopotamus, 3.5 m | 20,682 | 166,133 | 325,454 |
| african-bush-elephant, 6.0 m | 62,635 | 498,016 | **975,538** |

At 2.5 cm the moose and the bison land in the shipped band. The yak, rhino and
hippo sit at 2–3× it. The elephant is an outlier at any setting: half a million
voxels at 2.5 cm and nearly a million at 2 cm, against 50,339 for the most
expensive animal shipping today.

## 3. What was done — applied 2026-08-16 on the owner's approval

**25 of the 26 moved. The elephant stayed.** Every species was priced at its
target before anything was written, and the whole 26 went from 197,782 voxels
to 1,321,072 — 6.7× — for one individual each at seed 1.

| | count | lattice | voxels after |
|---|---|---|---|
| at or under 2.4 m | 15 | 5 cm → **2 cm** | 15,176 – 55,527 |
| over 2.4 m | 10 | 5 cm → **2.5 cm** | 30,353 – 166,133 |
| `african-bush-elephant` | 1 | **stays 5 cm** | 62,635 |

The 15 land in and below the 27k–50k band that 63 species already pay, so that
half is parity rather than a new cost. The 10 giants are dearer — hippo 166,133
and white rhino 141,167 are 3× the dearest animal shipping before today — and
that is the price of a 3.5 m animal that reads as an animal.

**The elephant stayed at 5 cm, and that is the one judgement call in here.** At
6 m it is already 120 voxels along the body — better resolved than a lion was at
5 cm, and better than a muskox is at 2 cm. Moving it costs 498,016 voxels at
2.5 cm, ten times the dearest shipped animal, to improve the one species that
was never lattice-limited. If it should move anyway, it is a one-line change and
a re-bake.

Not touched: the 42 species on 1 cm and the 63 already on 2 cm.

## 4. Two traps found on the way here

**`resolution_cm` silently substitutes.** It is a `kind="choice"` row whose menu
is `("10", "5", "2.5", "2", "1")`. An out-of-menu value falls back to the
default without a word: six species built at `"3"` returned voxel counts
**byte-identical** to 5 cm — elephant 62,635 both ways, hippo 20,682, rhino
18,308. A whole pass pricing a "3 cm middle option" was measuring the fallback.
`tools/quadlattice_ab.py` refuses to draw a cell whose resolved size differs
from the one asked for, and the general guard belongs with the `materials.head`
fix. This is the project's signature failure on the one row where it silently
multiplies an asset's cost by fifteen.

**A comparison sheet must hold physical size fixed.** The first lattice sheet
this project drew let each image size itself to its own grid, which drew the
5 cm oak *bigger* than the 2 cm one and made the coarse lattice look generous.
`quadlattice_ab.py` fixes a metre at 150 px in every cell of every row.

## 5. What this does not decide

The 42 species on 1 cm are untouched here and may have the opposite problem.
Ten of them are 20 cm animals — jerboa, pika, mole, common frog — which are
**20 voxels long whatever we choose**, because 20 cm is the species-size floor
the owner set and 1 cm is the finest lattice on the menu. No lattice decision
reaches them.

**The limb fit is now owed a re-measurement, and this is the debt in writing.**
Every ratio in `docs/quadruped-limb-regression.md` was measured with these 25
species on 5 cm. `reffit dead` in particular rewrote 68 rows to *what the
one-voxel radius floor was already drawing*, and that is a lattice-dependent
quantity by construction: at 2 cm the floor stops binding on most of them, so
those rows now mean something they did not mean when they were written. The
species that were lattice-limited are exactly the species that just moved, which
is the point of the move and also the reason the numbers cannot simply be
carried over. Re-measured immediately after the change in §7.

## 6. `quad.eye`, and why validation had to become idempotent first

**This was fixed before the lattice change, and the order was not incidental.**
`spec.validate` was not idempotent: `sm.patch(spec, {})` returned a **different
individual** on 192 of 828 specs, so any pass that patched and saved reseeded
them silently — `tools/plantfit.py fit --apply` among them. Changing 25 lattices
through a patch-and-save would have quietly reseeded a fifth of the library
alongside it, and the lattice renders would have been read as lattice effects.

**The cause was one row.** `quad.eye` was authored `1.0, 0.0, 3.0, 1.0` and is
the only one of 34 `kind="int"` parameters with float bounds. A spec that never
authored an eye loaded the default as `1.0`; `patch` coerced it to `1`. Identity
is a hash of the spec's canonical JSON, and `"1.0"` and `"1"` serialise
differently **while comparing equal in Python** — the two dicts are `==`, so no
value comparison anywhere in the codebase could ever have seen it. That is the
same shape as every other detachment in this project: a fact derived twice, in
two representations, agreeing until one of them moves.

**There was no cheaper repair, and that was checked rather than assumed.** 636
specs already carry `"eye": 1` on disk because they have been patch-and-saved at
some point; 192 do not. Normalising the hash in either direction moves one group
or the other, so 192 is the floor. Fixing the row's bounds pays exactly that
floor, once.

**Who actually reseeded is the surprise: not one of the 192 is a quadruped.**
Every quadruped spec had been saved through the coercion already. The 192 are 52
flowers, 44 grasses, 34 rocks, 20 birds, 12 fish, 10 reeds, 9 trees, 7 cetaceans
and 4 bushes — and the 9 trees and 34 rocks are terrain kinds with baked banks,
so the re-bake covers them.

`forge/cli.py selftest` now checks both ends every run: no `kind="int"` row may
carry a float bound (the cause), and re-validating any spec must return the same
individual (the symptom). The second matters more, because the next instance of
this will not be about integers.

## 7. Re-measured after the move, and the lattice was carrying a third of the error

`tools/reffit.py report`, same 78 species with a reference, same code both
sides. **No limb parameter was touched — only `resolution_cm`.**

| | before the move | after |
|---|---|---|
| limb thickness ÷ limb length, median | 1.29× life | **1.13×** |
| within 10% of life | 15 of 78 | **21 of 78** |
| belly clearance ÷ body length, median | 0.78× | **0.81×** |
| total height ÷ body length, within 10% | 20 of 78 | 21 of 78 |

**A third of the remaining limb error was the lattice**, and it came off for
free. `wood-bison` went 1.21× → 0.96×. That is the claim
`docs/quadruped-limb-regression.md` ended on — *"71 of 131 species are
lattice-limited and no value of any spec row can fix that"* — turning out to be
literally true and now literally paid.

**It also means the 68 rows `reffit dead` rewrote are due a re-fit, not just a
re-measurement.** Those rows record what the one-voxel radius floor was drawing
at 5 cm; at 2 cm the floor stops binding and the row becomes live again, so the
value that was a faithful description of the old drawing is now an authored
number nobody chose. The re-fit is `reffit dead` followed by `fit --thin` at the
new lattices, and it is deliberately NOT bundled into this change: the move is
worth reading on its own, and stacking a solver on top of it would make the two
impossible to tell apart in a render.
