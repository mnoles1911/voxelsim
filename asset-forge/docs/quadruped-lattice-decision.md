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

**The 20 species at or under 2.4 m, if moved to 2 cm:**

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

**The six over 2.4 m are a different question:**

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

## 3. The recommendation

* **The 20 species at or under 2.4 m → 2 cm.** Free, in the sense that it buys
  no cost the library is not already paying per animal, and it is the whole
  lattice-limited population among the mid-size animals.
* **The six giants → 2.5 cm**, which is the next value on the menu and puts the
  moose and bison in-band while roughly tripling the detail on the rest.
* **The elephant is the one genuinely open call.** 5 cm keeps it at 62,635 and
  120 voxels along the body — which, being 6 m long, is already better resolved
  than a lion at 5 cm. It may simply not need the move.

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

Nothing here re-fits any spec. Changing `resolution_cm` changes that species'
`spec_hash` and so reseeds it, and the limb ratios in
`docs/quadruped-limb-regression.md` were measured at the current lattices —
**they would all need re-measuring after a move**, and `reffit dead` in
particular rewrites rows to what the one-voxel floor draws, which is a
lattice-dependent quantity by construction.
