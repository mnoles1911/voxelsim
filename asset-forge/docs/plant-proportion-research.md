# Fitting plant proportions to measured references

Companion to `tools/plantprobe.py`, `tools/plantfit.py` and `tools/plantref.json`.
Written 2026-08-15, alongside the land-animal work in
`docs/reference-fitting-research.md`, and against the same brief: *use
measurements and imagery to fix all generations.*

**The gap being closed.** `docs/biomes/README.md` §8 says plainly that every
species size in the library is an unsourced approximation from general
knowledge. The tree specs were authored the same way — proportions judged
against other shipped specs, never against a real tree. This file replaces that
judgement for **one** ratio, and reports honestly on the other four.

**Plants are a better-measured subject than animals, and it showed.** The
animal work had to build a silhouette pipeline because nobody publishes a
dimensioned zebra. Foresters have published dimensioned trees for a century, at
scale, in the open: this exercise used **498,838 individually measured trees**
from one licence-clean database and **2,924,749 more** from a second, and never
needed a picture.

**Citation rule, inherited.** This project has shipped a fabricated citation
once. So every number below carries the URL or file it was read from; anything
recalled rather than read is marked **[estimate]**; and where a column's meaning
had to be established rather than read off, the method is given. Three separate
misreadings were caught in this session and each is written up in §3, including
one that produced a complete, plausible, entirely empty table.

**Licence rule, inherited.** Recorded per file, read off the artifact, never
inferred from the site.

---

## 1. What was built, in one paragraph

`tools/plantprobe.py` measures a **built voxel asset** the way a forest
inventory crew measures a tree — total height, mean crown radius from crown
projected area, height to live crown base, and stem diameter at 1.3 m — and
`tools/plantfit.py` solves `trunk.radius_base_m` until the last of those agrees
with published allometry. `tools/plantref.json` is the reference, 78 rows,
checked in; nothing under `forge/` imports any of it and no command in the
shipping path touches a network. The generator, the growth models, the crown
envelopes and the seeds are untouched: **only spec values moved, and only one
of them.**

---

## 2. Sources, with licences read per file

### 2.1 ADOPTED — Tallo (the tree work rests on this)

`https://zenodo.org/api/records/6637599`. Licence read from the record API,
verbatim field: `"license": { "id": "cc-by-4.0" }`. Version 1.0.0, published
2022-06-13, creators Jucker, Fischer, Chave *et al.* The accompanying paper is
*Tallo: A global tree allometry and crown architecture database*, Global Change
Biology, `10.1111/gcb.16302`; Crossref reports its licence as
`http://creativecommons.org/licenses/by/4.0/`. **CC BY, not NC and not SA**, so
it is usable here and the obligation is attribution.

`Tallo.csv`, 49,059,652 bytes, **498,838 trees, 5,164 species**. The fields
used, quoted from `Tallo_metadata.csv` shipped in the same record:

| field | description, verbatim |
|---|---|
| `stem_diameter_cm` | "Stem diameter (in cm)…" |
| `height_m` | "Tree height (in m)" |
| `crown_radius_m` | "Crown radius (in m)" |
| `height_outlier` / `crown_radius_outlier` | "Y = outlier; N = non-outlier" |

Only records the dataset itself does **not** flag as outliers are used.

**How it is used.** Not as a lookup of "how big is a beech" — as a **power law
per species**, `y = a·H^b`, fitted by ordinary least squares in log-log with the
Baskerville back-transform correction, separately for crown radius on height
and stem diameter on height. The reference is then evaluated **at our own
asset's built height**, so the comparison is a proportion and not an argument
about which individual we are depicting. A fit is only used at **n ≥ 40 and
r² ≥ 0.20**; that gate drops species like `white-poplar` (r² 0.03, 43 trees over
a 5 m height range) whose slope is noise.

### 2.2 ADOPTED — USDA FIA, as the independent cross-check and for crown ratio

`https://apps.fs.usda.gov/fia/datamart/CSV/…`. A work of the US Government;
public domain. Downloaded: `REF_SPECIES.csv` and the `TREE` table for **AZ, VT,
NH, WV, IN, WA, CA, FL, NM** — nine states, **2,924,749 live stems** (FL 789,613,
WA 407,829, CA 357,235, WV 323,504, AZ 229,555, IN 227,182, NH 210,384,
NM 207,563, VT 171,884).

Fields used: `SPCD` (species), `STATUSCD` (1 = live), `HT` (total height, feet),
`DIA` (diameter at breast height, inches), `CR` (**compacted** live crown ratio,
percent), `UNCRCD` (**uncompacted** live crown ratio, percent), `CCLCD` (crown
class; **1 = open grown**).

FIA is the only source found that publishes **height to live crown base** per
tree at scale, which is the quantity `crown.height_frac` is trying to be.

### 2.3 ADOPTED — USDA Forest Service FVS variant overviews, for open-grown crown width only

`https://www.fs.usda.gov/fmsc/ftp/fvs/docs/overviews/FVSne_Overview.pdf` and
`FVSls_Overview.pdf`. US Government, public domain. These reprint, with
coefficients per species, the **open-grown** crown width equations of Ek (1974),
Krajicek and others (1961) and Smith and others (1992). Units are DBH inches,
crown width feet, stated in the document.

**Only the open-grown tables are used, and §3.2 is why.**

### 2.4 The cross-check that makes the headline finding safe to act on

Before rescaling 52 species, the reference was checked against a completely
independent one. Stem diameter predicted at a height of 20 m, Tallo's power law
against a power law fitted the same way to FIA's own `HT`/`DIA` columns:

| species | n (FIA) | FIA, cm | Tallo, cm | FIA/Tallo |
|---|---|---|---|---|
| *Acer saccharum* | 64,655 | 26.8 | 26.9 | 1.00 |
| *Fagus grandifolia* | 41,470 | 30.2 | 28.0 | 1.08 |
| *Pseudotsuga menziesii* | 185,061 | 30.7 | 26.7 | 1.15 |
| *Pinus strobus* | 16,559 | 31.1 | 30.8 | 1.01 |
| *Thuja plicata* | 24,804 | 36.2 | 34.8 | 1.04 |
| *Tsuga heterophylla* | 71,261 | 28.7 | 29.8 | 0.96 |
| *Liriodendron tulipifera* | 19,813 | 24.2 | 25.2 | 0.96 |

**17 species, median FIA/Tallo = 0.96.** Two datasets built by different people
on different continents agree to 4% on how thick a tree of a given height is.
That is what licenses a bulk change.

### 2.5 The open-grown allowances, measured rather than assumed

Both reference datasets are dominated by **forest plots**, and a forest tree is
thin for its height because it is racing its neighbours. These assets are
scattered detail entities, closer to open-grown. So the allowance was measured,
not guessed — FIA records a crown class per tree, and code 1 is "open grown":

| quantity | open-grown ÷ stand-grown | how |
|---|---|---|
| **stem diameter at 15 m** | **1.50×** (range 1.27–2.19, n=19 spp) | FIA `CCLCD=1` vs all, same power law fitted to each, species with ≥80 open-grown stems |
| **crown width at DBH 50 cm** | **1.20×** (range 0.63–1.82, n=19 spp) | FVS open-grown equations vs Tallo's crown-radius law |
| **crown ratio** | **1.08×** (n=17 spp) | FIA `UNCRCD`, `CCLCD=1` vs all |

`OPEN_GROWN = 1.50` in `tools/plantfit.py` is that first row and nothing else.

### 2.6 ADOPTED AS A SCREEN ONLY — USDA PLANTS Database

`https://plantsservices.sc.egov.usda.gov/api/PlantCharacteristics/{id}`, reached
via `…/api/PlantProfile?symbol={symbol}`. US Government, public domain. The row
used is `Height, Mature (feet)`.

It resolved **64 of 157** ground-cover, flower, reed and bush species tried. It is used
in §6 to *screen* the herbaceous library, and it is **rejected as a fitting
target** — see rejection 5.

**A trap, handled.** PLANTS symbols are algorithmic (two letters of genus, two
of species, a digit on collision) and therefore guessable **and wrong**:
*Lotus corniculatus* is `LOCO6`, and `LOCO` is a different plant. So every
candidate symbol is fetched and the returned `ScientificName` is checked to
start with the binomial that was asked for; a mismatch is discarded. That is
the same defence `tools/refnames.py` puts in front of the animal references, and
for the same reason — the GBIF `lion` → *Macaca silenus* failure passed every
test downstream of it.

### 2.7 REJECTED, numbered

1. **TRY Plant Trait Database — not pursued, and not claimed as read.** Access
   is by request and the licence is **per contributing dataset**, many of them
   non-commercial. Nothing was downloaded and no licence file was read, so
   nothing here cites it. Recorded so the next person does not assume it was
   overlooked.

2. **NEON Vegetation Structure (`DP1.10098.001`) — mechanically unavailable.**
   This is the one product that publishes `baseCrownHeight` *and*
   `maxCrownDiameter` *and* `stemDiameter` per individual, which would have
   given crown ratio and crown width from one measured source. The product
   metadata endpoint answers normally (42 sites, months back to 2015-07); the
   **data endpoint returns 403 Forbidden** to a scripted request, with and
   without a browser user-agent. Not a licence refusal — a door that would not
   open. Worth retrying by hand.

3. **BAAD (Biomass And Allometry Database) — not obtainable licence-clean.**
   `traitecoevo/baad.data` exists on GitHub but its licence field reads
   `NOASSERTION`, it has **no releases**, and the only files in its tree are
   `README.md`, `.travis.yml` and `appveyor.yml` — the data is assembled at
   build time from sources not in the repository. Zenodo searches for it
   returned unrelated records. Nothing was taken from it.

4. **FVS *forest-grown* crown-width table (Bechtold 2003) — rejected for
   quantitative use.** Not on licence; on extraction reliability. See §3.2:
   after the column bug was fixed, three species still produced a forest-grown
   crown wider than their own open-grown crown, which is physically backwards
   and means at least those rows are still being read wrong. Since Tallo already
   covers stand-grown crown width from measured trees, the fragile table is not
   needed and is not used. Only the open-grown equations, whose coefficients are
   contiguous and therefore unambiguous, are kept.

5. **USDA PLANTS `Height, Mature` — rejected as a fitting target.** The number
   is real and public domain, but its **definition is not constant across
   growth forms**, and fitting to it would have written errors into the specs:

   - *Echinacea purpurea* is filed at **0.37 m**. Purple coneflower in flower is
     0.6–1.2 m; 0.37 m is its basal foliage. Fitting would have cut our
     `purple-coneflower` by two thirds to match a leaf rosette.
   - *Hedera helix* is filed at **1.52 m**. Ivy as a ground layer is ankle-high;
     as a climber it reaches 20 m. Neither number is 1.52.
   - *Ajuga reptans* is filed at **0.09 m** — the mat, not the flower spike.

   So PLANTS answers "how tall is the plant" with a different question per
   species, and a fit against it would be precise about the wrong thing. It is
   used only to find specs that are *badly* out, and §6 reports those without
   changing them.

---

## 3. The measurement, and the four ways it was wrong first

`tools/plantprobe.py` reduces a built voxel grid to the four numbers a field
crew records. Every one of them needed a definition argument settled before the
comparison meant anything.

### 3.1 Crown radius is a MEAN, and a bounding box is not a mean

Tallo stores `crown_radius_m`, a mean crown radius. The obvious voxel analogue —
half the widest horizontal extent of the asset — is **not the same quantity**: a
lopsided crown with one long branch has a bounding box far wider than its mean
radius, so comparing the two reports a crown-width error that is really a
definition difference. This is the same trap `docs/reference-fitting-research.md`
§3.1 hit from the other side with limb length.

The probe now computes **crown projected area** — the plan-view union of
occupied columns above the crown base — and takes `sqrt(CPA/π)`. Holes are
filled first, because a field crew reads the crown edge off the drip line and a
gap between two foliage clumps is inside the crown, not outside it; unfilled, a
loosely-foliaged pine reports a porosity measurement.

The correction is not cosmetic: it moved the library-wide crown-width verdict
from **1.64× to 1.50×**.

### 3.2 A blank cell in a PDF table slides every coefficient left

The FVS crown-width tables have optional terms — a species whose equation has no
DBH² term leaves column `a3` empty. `pypdf`'s `extract_text()` **drops the
blank**, so the remaining coefficients shift left and land in the wrong slots.

Read that way, balsam fir's crown-ratio coefficient `0.0792` became its DBH²
coefficient and its predicted forest-grown crown width came out **49.1 ft
instead of 21.0 ft**. The table then said open-grown trees have *narrower*
crowns than forest-grown ones — 0.02× for balsam fir — which is backwards, **and
that is the only reason it was caught**. Every individual number in it looked
like a plausible forestry coefficient.

The fix is to recover columns from **x-coordinates** rather than token order:
the header row's five `a` glyphs give the column centres, and each numeric token
goes to the nearest one. That fixed the forest-grown table for most species but
not all (§2.7 rejection 4), so only the open-grown equations — which have no
optional middle term and are therefore unambiguous in order — are trusted.

### 3.3 `SPCD` is written `134.0`, and the empty table looked fine

The FIA crown-ratio scan reported **"0 live trees"** for all nine states and
printed a complete, correctly formatted, entirely empty table. The cause:
`SPCD` ships as `"134.0"`, not `"134"`; `int()` on that raises `ValueError`; the
`except: continue` sat in front of the row counter; so every row was skipped and
the counter never advanced.

This is the library's signature failure — the silent no-op — and it was found
only because "0 live trees" is obviously impossible. A subtler filter would have
produced a smaller, wrong, believable answer.

### 3.4 `spec.patch` returns a new spec; it does not mutate

The first trunk-thinning trial printed **identical before and after numbers for
six species across three seeds each** — 18 builds, all reporting no change.
`sm.patch(spec, {...})` returns `(new_spec, Report)`; calling it for its side
effect changes nothing. Eighteen builds of evidence that a change did nothing,
which is exactly what a working guard against silent no-ops looks like, and
exactly what would have been mistaken for "the parameter has no effect".

`tools/plantfit.py` now also **re-reads the value back after patching** and
refuses to save if `validate` clamped it to something other than what was asked.

### 3.5 Compacted and uncompacted crown ratio are not the same number

FIA carries both. `CR` is *compacted* — the crew mentally pushes branches in to
fill holes, giving a fuller, shorter crown. `UNCRCD` is the honest base-of-live-
crown to top. Across 125 species with n ≥ 100, **median `UNCRCD`/`CR` = 1.33**
(species-median compacted 40%, uncompacted 55%).

Only `UNCRCD` is comparable to "where do the leaves start" on a voxel asset.
Taking the more prominent `CR` column would have been a **33% error in the
direction of concluding our crowns are far too deep**.

Measured, 125 species with n ≥ 100: **uncompacted crown ratio median 55%, range
25% to 98%.** Aspen 35%, red maple-country hardwoods 40–55%, woodland junipers
and pinyons 90–99% because their crowns run to the ground. **Crown ratio is
another quantity where nature has a range and one number cannot serve.**

### 3.6 A genus fallback for crown ratio is a lion-to-macaque waiting to happen

Only **12 of 78** tree specs have a species-level FIA crown ratio. Falling back
to the genus median fills in another 27 — and for *Pinus* the FIA genus is
dominated by pinyon and one-leaf pines from the Arizona woodland plots, whose
crowns reach the ground at 86–99%. Used as a reference for `stone-pine` and
`maritime-pine` — Mediterranean pines whose own notes author a deliberately bare
four-fifths of bole — it declares them 70% wrong.

They are not wrong; the reference is. §4 therefore reports crown ratio **twice**
— once over the 12 species-level matches, which is the number to believe, and
once over all 39 including the genus fallback, which is the number a less
careful pass would have published. **Nothing was fitted from either.**

---

## 4. How far the tree specs were from the measurements

Ours = the **built asset**, mean of **seeds 1, 2 and 3**. A single seed is not
enough: the shipped `douglas-fir` measures DBH **68.6 / 93.7 / 78.2 cm** across
three seeds of one spec — a 37% spread — because `variation.trunk_radius` moves
it per individual and a 10 cm lattice then quantises what is left.

The reference is evaluated at our own built height. **64 of 78** tree specs have
at least one usable reference — **46 at species level in Tallo and 18 only at
genus level**, and §3.6's warning applies to those 18 as well (see below).

| ratio | n | median ours ÷ ref | range | mean abs error | within 10% |
|---|---|---|---|---|---|
| height vs species p90 | 64 | **0.98×** | 0.11–3.39× | **35.9%** | 9/64 |
| height vs species p95 | 64 | 0.89× | 0.10–1.73× | 30.9% | 15/64 |
| crown width / height, vs stand-grown | 42 | **1.50×** | 0.76–3.23× | **63.7%** | 8/42 |
| crown width / height, vs open-grown (÷1.20) | 42 | **1.25×** | | | |
| **stem diameter / height, vs stand-grown** | 55 | **3.05×** | 1.35–15.80× | **300%** | **0/55** |
| **stem diameter / height, vs open-grown target** | 52 | **2.00×** | | **134%** | **4/52** |
| **crown ratio, vs FIA uncompacted, species-level only** | **12** | **1.15×** | 0.90–1.83× | **22.3%** | 5/12 |
| crown ratio, same but including genus fallback (§3.6 — do not believe this row) | 39 | 1.15× | 0.27–2.07× | 34.0% | 11/39 |
| built height ÷ authored `height_m` | 64 | 1.00× | 0.82–1.22× | 7.6% | 48/64 |
| trunk taper d(1.3 m)/d(0.1 m), ours | 64 | 0.83 | 0.53–1.71 | | |

**The answer to "how far off are we" is: three of the four are close in the
median and scattered around it, and the fourth is wrong by a factor of two in
one direction for essentially everything.**

**Height — the level is right and the SPREAD is wrong, exactly as with the
animals.** Median 0.98× of the species' 90th-percentile height, which says the
authoring judgement landed on life size on average; mean absolute error 35.9%
and only 9 of 64 within 10%, which says almost no individual species did.
`hero-sequoia` at 3.39× is deliberate.

**But the two worst-looking height errors are the REFERENCE, not the spec, and
this is the §3.6 trap again in a second place.** `japanese-maple` reads 0.20×
and `sea-grape` 0.34× — both are among the 18 species with only a genus-level
Tallo fit. *Acer palmatum* is a 5–10 m understorey tree and the *Acer* genus in
Tallo is dominated by big canopy maples with a 90th-percentile height of 23.6 m;
*Coccoloba uvifera* is a coastal shrub-tree and its genus runs to rainforest
trees. **Neither of those two numbers is evidence about our specs**, and it is
the reason nothing in this pass fitted height. Whenever a genus stands in for a
species, the reference is a different plant with the same first word.

**Crown width — 25% too wide after every allowance that could be sourced.**
1.50× against measured stand-grown trees; 1.25× against the open-grown figure
measured in §2.5. A quarter too wide is inside the range of "which individual",
and the crown envelope is also the parameter the owner has tuned by eye across
several passes. **Measured, reported, not changed.**

**Crown ratio — median 1.15×, and the reference is too thin to act on.** On the
12 species with a species-level FIA match, our crowns are 15% deeper than
measured with a 22% mean absolute error — close, and the same "level right,
spread wrong" shape again. But 12 species is not a basis for moving 78, and
§3.6 shows the genus fallback that would fill the gap actively lying about
Mediterranean pines. **Measured, reported, not changed.**

**Stem diameter — the finding.** Median **3.05×** the stem a real tree of that
species carries at that height, and **not one of 55 species was within 10%**.
Against the open-grown target — which is 1.50× more generous still — it was
**2.00×**, with 4 of 52 within 10%. Unlike the other three ratios this one is
**one-directional**: 52 of 55 species were too thick, and the three that were
not (`quaking-aspen`, `savanna-acacia`, `wild-pear`) were too thin.

Two independent national inventories agree to 4% on the target (§2.4). The
library's trunks were, plainly, about twice as thick as trees.

---

## 5. What was fitted

### 5.1 The fit

`tools/plantfit.py fit --apply`, four passes to convergence (§5.4 explains why
one pass is not enough). **52 of 78 tree specs** had `trunk.radius_base_m`
changed. **Nothing else in any spec was touched by this exercise**, verified by
diffing all 828 specs against a snapshot: the only field this pass wrote is
`trunk.radius_base_m`.

Median scale **0.55×**. The extremes: `weeping-willow` 0.460 → 0.101 m (0.22×),
`jackalberry` 0.700 → 0.168 m, `wych-elm` 0.550 → 0.148 m; and in the other
direction `quaking-aspen` 0.180 → 0.215 m (1.19×), `wild-pear` 0.160 → 0.178 m,
`savanna-acacia` 0.340 → 0.346 m. **That three species got thicker is the
evidence the fit is per-species and not a blanket shrink.**

### 5.2 Before and after, the number that had to move

Measured the same way, three seeds, against the open-grown target:

| | n | median ours ÷ target | mean abs error | within 10% | within 25% |
|---|---|---|---|---|---|
| **before** | 52 | **2.00×** | **134%** | 4/52 | 6/52 |
| **after** | 52 | **1.03×** | **20.1%** | **36/52** | **45/52** |

Against the raw stand-grown Tallo law, the library moved from **3.05× to
1.57×** — i.e. onto the 1.50× open-grown figure, which is where it was aimed.

The other ratios, re-measured after the change, moved as expected — barely:
height 0.98× → 0.96×, crown width 1.50× → 1.56×, crown ratio 1.15× → 1.11×.
Trunk thickness and crown shape are close to independent in this generator,
which is the result that says the fit did not buy its win somewhere else.

### 5.3 Five species deliberately NOT fitted, each with its reason read from its own notes

The animal work's lesson was that a constant cannot serve where nature has a
range. The same error is available in reverse — flattening a real outlier to an
average — so five species are excluded in code, with the reason in
`plantfit.EXCLUDED`:

| species | why |
|---|---|
| `baobab` | its notes: *"Enormous water-storing trunk under a small sparse crown … the trunk is most of the tree."* True of *Adansonia*. |
| `european-yew` | notes author the 1.1 m radius as *"what many fused stems look like from outside"*. Ancient yews reach it. |
| `olive` | notes: *"THE GNARL IS THE SPECIES"* — the flared hollow base is the whole silhouette. |
| `strangler-fig` | the base is **fused root, not stem**; its notes put `roots.count` and `roots.rise` both at their ceilings to get it. A stem-diameter law does not describe it. |
| `hero-sequoia` | a hero the owner sized. Its measured 12.1 m base is close to the largest living sequoias' ground diameter **[estimate — not sourced in this session]** and is not an accident. **Its taper is a separate finding** — see §5.4. |

A further 21 species have **no stem-diameter reference at all** and were skipped
rather than guessed: the palms (`coast-palm`, `date-palm`, `doum-palm`), the
non-woody and non-dicot forms (`saguaro`, `joshua-tree`, `tree-fern`,
`wild-banana`, `screwpine`), the four generics that name no species
(`desert-dead`, `jungle-emergent`, `river-broadleaf`, `temperate-sapling`), and
species Tallo simply does not carry at n ≥ 40 (`crab-apple`, `hawthorn-scrub`,
`monterey-cypress`, `desert-ironwood`, `beach-hibiscus`, `sausage-tree`,
`sweet-chestnut`, `white-poplar`, `alpine-krummholz`).

### 5.4 Where the generator cannot express the real proportion

Four of these were found by fitting, not by reading code.

1. **Trunk taper is not a function of height, so a tree that forks little is a
   post.** `skeleton._radii` derives every node's radius from Murray's law over
   the branching topology and then rescales so the root equals
   `trunk.radius_base_m`. Taper is therefore *emergent from where branches fork*
   and there is no `d(z)` anywhere. Measured across the library, d(1.3 m)/d(0.1 m)
   spans **0.53 to 1.71**. `birch` measures **1.00 on all three seeds** — a
   perfectly cylindrical bole — and `hero-sequoia` measures **0.977 / 0.977 /
   0.988**, i.e. a 90 m untapered column. Real stems taper, and the honest fix
   is a taper term in the generator, not a spec value. **This is the one finding
   here that a spec value cannot reach.**

   A caveat on the other end of that range: the thinnest boles after the fit sit
   at one or two voxels, where a diameter is quantised to 10 cm steps and a
   taper ratio stops meaning anything — `columnar-cypress` now reports 1.095 on
   one seed, i.e. wider at breast height than at the ground, which is
   quantisation and not shape.

2. **The root flare's reach scales with tree height, so on a tall tree it
   swallows breast height.** `skeleton.py` spends the buttress flare over the
   bottom **one eighth of `height_m`**. On a 24 m beech that is 3 m — breast
   height at 1.3 m is entirely inside the flare, so `trunk.buttress` inflates
   measured DBH instead of sitting below it. Real butt swell is spent within
   about a metre [estimate]. **This is why the fit needed four passes**: each
   change to the base radius moved DBH by a factor the flare made non-linear.

3. **The 10 cm terrain lattice floors trunk diameter at about 20 cm.** A radius
   below 0.10 m is a single voxel and the bole stops being a cylinder. Six
   species want thinner stems than that and are floored, not fitted:
   `japanese-maple`, `cherry-blossom`, `sea-grape`, `flowering-dogwood`, `rowan`
   and `columnar-cypress`. `japanese-maple` builds at **19.7 cm DBH on a 5.0 m
   tree**; a real 5 m *Acer palmatum* is nearer 8–12 cm **[estimate — its
   reference here is a genus-level fit extrapolated down from canopy maples and
   is not trustworthy at 5 m, see §4]**. Either way the floor, not the
   reference, is what sets it. Each of these is a small tree whose trunk now
   sits at the same one-voxel floor `japanese-maple`'s own notes already record
   for its twigs.

4. **`height_m` is not the tree's height under `colonize`.** The top of the
   tree is wherever the crown envelope reaches — `center_frac + height_frac ·
   squash / 2` times `height_m` — and nothing enforces the authored number.
   Measured median 1.00× but range **0.82–1.22×**. Reported rather than fixed
   because changing it would move every crown in the library; but it means the
   authored `height_m` should not be read as a promise.

A fifth, noticed while reading: `envelope.py`'s allometry mode converts base
radius to DBH with a **hardcoded 0.85**, commented in the source as *"a taper
allowance, not a measurement"*. The library's measured median taper is **0.83**,
so the guess was good — but it is a constant standing where a 0.53–1.71 range
lives, and it only bites on the species that switch allometry on.

---

## 6. The herbaceous half: 89 grasses, 87 flowers, 33 reeds, 57 bushes

Kept separate because the finding is different in kind.

**A counting note first.** The brief asked for 47 grasses, 66 flowers, 14 reeds
and 49 bushes — 176 specs. The library on disk holds **266**: 89 `grass`,
87 `flower`, 33 `reed`, 57 `bush`. The difference is the marine and freshwater
set, which lives in the same four kinds — kelps and wracks are authored as
`reed`, seagrasses and turf algae and anemones as `grass`, corals as `bush`.
All 266 were probed; none of them has a reference here (§8).

### 6.1 Two structural facts before any comparison

**`height_m` for a tuft is stem ARC LENGTH, not plant height.** `forge/ground.py`
traces each blade for `height_m` of path along a curve that tilts toward
horizontal, and a head is then drawn on top. Measured over all 266 ground-cover,
flower, reed and bush specs at three seeds, **built height ÷ authored `height_m`
= median 1.13×, mean absolute error 16.4%, range 0.60–1.57×**. The authored
number is systematically not what ships.

**73 of the 266 are authored at or below 0.30 m and 46 of those say in their own
notes that the floor, not the plant, set the number.** For those, height is not
a biological quantity at all. `buffalo-grass`:
*"AUTHORED AT 0.22 m AGAINST THE BIOME LIST'S APPROXIMATE 0.15 m. At 5 cm
0.15 m is three voxels and a turf-forming grass has no shape at three voxels."*
`channelled-wrack` prints its own arithmetic: *"about 0.10–0.15 m in life … the
owner's floor is 0.20 m, so it is authored at 0.22 — roughly 1.7× life size."*
**Nothing in this pass corrected any of them back**, per the brief.

### 6.2 How far off, screened against USDA PLANTS

64 species resolved with a verified binomial and a mature height on file:

| | n | median ours ÷ PLANTS | range | mean abs error | within 25% |
|---|---|---|---|---|---|
| all matched | 64 | **1.00×** | 0.14–2.91× | **40.7%** | 25/64 |
| excluding specs at the 0.30 m floor | 49 | **1.00×** | 0.37–2.91× | **36.2%** | 22/49 |

**The same signature as the animals and as tree height: the level is right, the
spread is not.** A median of exactly 1.00× across 64 species says the authoring
judgement was calibrated; a 36–41% mean absolute error says almost no individual
species was.

### 6.3 Why nothing here was changed

Because a good part of that 36% is the **reference**, not the specs — §2.7
rejection 5 shows three worked cases where PLANTS' `Height, Mature` answers a
different question per growth form. The largest apparent errors are exactly the
species where that bites: `purple-coneflower` 2.91× (PLANTS files basal foliage),
`bugle` 2.75× (mat, not flower spike), `ivy-ground-layer` 0.14× (a climber's
height for a ground layer).

Fitting to that would encode the source's ambiguity into 266 specs. **The
honest recommendation is to stop here for ground cover** until a trait source
with a stated measurement protocol is available — LEDA, BiolFlor, PLANTATT and
the national floras are all candidates and none was reachable licence-clean in
this session. What §6.2 does establish is that ground cover is **not** the place
the library is broken.

---

## 7. Verification

    python -m forge.cli selftest      -> selftest: PASS   (824 specs)
    python tools/buildcheck.py        -> buildcheck: PASS  (828 builds in 786 s)
    python tools/buildcheck.py --kind tree --seeds 1 2 3 4   -> see below

Both match the baseline exactly. The four-seed tree run is the one that matters
for this change: a defect that shows on one individual in eight is invisible at
seed 1, and that is how the kelp and sequoia bugs survived.

Probes and reports, all checked in under `out/`:

| file | what |
|---|---|
| `out/plantprobe-tree-before.txt`, `out/plantprobe/tree-before.json` | 78 trees × 3 seeds, before |
| `out/plantprobe-tree-after.txt`, `out/plantprobe/tree-after.json` | the same after |
| `out/plantprobe-ground-before.txt`, `out/plantprobe/ground-before.json` | 266 ground-cover/bush/flower/reed × 3 seeds |
| `out/plantfit-pass1..4.txt` | every value the fit moved, pass by pass |

Renders. The owner judges these; no verdict on them is offered here.

| file | what |
|---|---|
| `out/plantfit-ab-all-seed1.png` | all 52 changed species, before over after, seed 1 |
| `out/plantfit-ab-all-seed3.png` | the same at seed 3 |
| `out/plantfit-ab-seed1.png` | a six-species close-up of the same pairs |
| `out/plants-tree.png` | contact sheet, all 78 trees, one scale |
| `out/plants-bush.png`, `plants-grass.png`, `plants-flower.png`, `plants-reed.png` | contact sheet per remaining kind |

Each before/after pair is rendered at **one locked scale**: `tools/plantsheet.py`
computes the pixels-per-voxel from whichever of the two builds is larger and
passes it to both, then applies **one** resize factor to the pair, and
bottom-aligns them on a common ground line. Fitting each tile to its cell
separately — which is what `tools/crown_ab.py` does — silently undoes the
comparison, because a tree that got thinner also gets a larger fit factor and
comes back looking identical.

**Three seeds throughout, never one.** A three-seed sweep found exactly one
defect in 2,472 builds yesterday, and the same lesson bites here on the
measurement side: `douglas-fir` measures DBH 68.6 / 93.7 / 78.2 cm across three
seeds of one spec. A one-seed number would have been noise, and the fit is
driven by these numbers.

---

## 8. What could not be sourced, stated plainly

- **Crown depth for anything outside North America.** Only 12 of 78 tree specs
  have a species-level crown ratio, all FIA, all US. The European and tropical
  species have none, and §3.6 shows the genus fallback is actively wrong for
  Mediterranean pines. This is the biggest remaining hole and NEON (rejection 2)
  is the obvious next door to knock on.
- **Trunk taper, for any species.** No taper equation was obtained. The library's
  0.53–1.71 spread is unjudged: it is measured against nothing.
- **Bush and shrub crown width.** Tallo is trees; shrubs are absent. Nothing in
  §4 applies to the 57 bushes, and no shrub-architecture source was found.
- **Everything marine.** Kelps, wracks, seagrasses, corals and anemones sit in
  the `grass`/`reed`/`bush` kinds and have no reference here at all.
- **All 89 grasses' and 87 flowers' individual heights.** §6 screens them; it
  does not fit them, and the residual 36% is shared between the specs and a
  source whose measurement protocol is not stated.
- **The 21 tree specs of §5.3** with no stem-diameter reference — including all
  three palms and every generic that names no species. Their trunks are still
  authored by judgement and were not touched.
- **18 of the 64 references are genus-level, not species-level**, and §4 shows
  two of them (`japanese-maple`, `sea-grape`) producing height errors that are
  the reference's fault rather than the spec's. Their *stem-diameter* fits were
  still applied, on the argument that the direction is unambiguous — every
  reading, at every level, said the trunks were far too thick — but a
  genus-level fit extrapolated to a small understorey tree is the weakest link
  in §5 and is named here rather than buried.
- **The open-grown allowances are North American.** 1.50× for stem diameter and
  1.20× for crown width are measured from FIA and FVS, i.e. from US species and
  US stand conditions, and applied to European and tropical species as well.
  That is an assumption, and it is the largest single one in §5.
