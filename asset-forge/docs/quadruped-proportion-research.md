# Why the land animals looked lanky, and what the right proportions are

Companion to `forge/quadruped.py` and `tools/quadprobe.py --bulk`. Written
2026-08-15, after the owner's verdict on the first full land-animal library:

> "All of the quadruped land animals do not look particularly realistic or
> good. They look tall lanky with narrow legs and bodies."

**Citation rule for this file.** This project has shipped a fabricated
citation once (an orca eye patch "measured" from what were dimensionless
indices in an unrelated paper) and a second agent independently found a
morphometrics table printing percentages under headings that read like
millimetres. So: every number below that is attributed carries the URL it was
read from, and anything that is a recollection rather than a reading is marked
**[estimate]** at the point of use. Where a source's units had to be
established rather than read off, §2 says how.

---

## 1. The measurement that found it

Nothing in `tools/quadprobe.py` could see this defect, and that is the first
finding. The parameter sweep proved every one of the eighty `quad` rows moves
a number. `--stance` proved all four feet reach the floor on all 131 species.
`--parts` proved four legs are four rigged parts. `--caps` accounted for every
joint ball. **All four reported clean on the day the owner said the animals
looked wrong.**

The gap is that no measurement compared one part of an animal to another part
of the same animal, and "lanky" is a statement about exactly that comparison.

### 1.1 Two plausible ratios that do NOT separate the good from the bad

The owner named three species that read as solid animals (`american-bison`,
`brown-bear`, `wild-boar`) and five that read as wireframes (`gemsbok`,
`greater-kudu`, `moose`, `plains-zebra`, `red-deer-stag`). That is a labelled
set, so any proposed metric can be tested against it before it is trusted.

**REJECTED — limb thickness in voxels.** Measured across one foreleg:

| accepted | vox | rejected | vox |
|---|---|---|---|
| american-bison | 5.0 | gemsbok | 3.0 |
| brown-bear | 4.0 | greater-kudu | 3.0 |
| wild-boar | 3.0 | moose | 3.0 |
| warthog | 3.0 | plains-zebra | 5.0 |
| | | red-deer-stag | 3.0 |

`wild-boar` and `warthog` are accepted at 3.0; `plains-zebra` is rejected at
5.0. The groups overlap completely. A raw voxel count cannot be the thing.

**REJECTED — limb thickness over withers height.** On the earlier
variation-on measurement, `wild-boar` (accepted) came out at **0.055** and
`gemsbok` (rejected) at **0.050** — the same number on one animal the owner
liked and one he did not. Rejected on that basis.

### 1.2 ADOPTED — limb thickness over limb LENGTH

| accepted | t/L | rejected | t/L |
|---|---|---|---|
| brown-bear | 0.364 | moose | 0.107 |
| american-bison | 0.250 | plains-zebra | 0.104 |
| warthog | 0.136 | gemsbok | 0.064 |
| wild-boar | 0.115 | red-deer-stag | 0.060 |
| | | greater-kudu | 0.055 |

Two groups, no overlap, with the boundary between 0.107 and 0.115. Measured
by `tools/quadprobe.py --bulk` on the trunk-relative-limb geometry as it
shipped, variation pinned off. This is also the ratio the published creature
sets in §2 are quoted in, so it can be compared outward as well as inward.

**Why the generator produced it.** `quad.leg_thick` was limb diameter as a
fraction of the TRUNK'S DEPTH, and trunk depth is three multiplications away
from anything the eye judges:

```
leg_r   = leg_thick × depth_v × 0.5
depth_v = quad.depth × trunk_v
trunk_v = (trunk_frac ÷ the four length shares) × length_v
```

Every factor is below 1, and the species that need the thickest legs shrink
all three. A gemsbok is a quarter neck, so its trunk share is 0.52 where a
boar's is 0.58; a running build is shallow, so its `quad.depth` is 0.42 where
a boar's is 0.46. **Nothing in that chain knows how long the leg is** — and
the leg is long precisely because the animal is tall. So the taller the
animal, the thinner its legs came out, which is the definition of lanky and is
exactly the set of species the owner picked out.

### 1.3 A measurement defect found and fixed during this work

`m_trunk_girth` first sampled the trunk at 0.82 of its length — where a
livestock heart girth is taken, just behind the elbow. That is also exactly
where `_limb_caps` draws the shoulder ball, and a joint ball carries the
TRUNK's tag by design (`forge/quadruped.py`: a voxel belonging to two parts
has no defined answer when both rotate).

Sweeping `quad.leg_thick` from 0.10 to 0.40 with nothing else changed:

| station | gemsbok | american-bison | plains-zebra |
|---|---|---|---|
| 0.82 (behind the elbow) | 63.8 → 110.0 | 59.9 → 74.2 | 74.5 → 122.5 |
| 0.55 (mid barrel) | 52.8 → 52.8 | 47.2 → 46.7 | 60.7 → 62.1 |

"Chest girth" was measuring the legs. A tool solving limb thickness and trunk
bulk against each other would have chased its own tail. The measurement moved
to mid-barrel, and it is therefore **not** a heart girth and is not
interchangeable with the published values in §3 — on a real grazer the barrel
is the fuller of the two.

**This also overturned an earlier reading of my own.** On the contaminated
station, girth/withers appeared to separate the owner's two groups cleanly
(accepted 1.22–1.41, rejected 0.82–0.91), and I had it written down as the
diagnosis. On the clean station the accepted group is 0.88–1.12 and the
rejected 0.64–0.78 — overlapping at the `wild-boar` (0.88) / `plains-zebra`
(0.78) boundary. **Trunk girth is a real deficiency against live animals (§3)
but it is not what distinguishes the species the owner rejected.** Limb
slenderness is.

---

## 2. Voxel and low-poly art convention — how much the genre exaggerates

### 2.1 Minecraft

Cube sizes read from the Bedrock reference models, e.g.
`https://raw.githubusercontent.com/Mojang/bedrock-samples/main/resource_pack/models/entity/cow.geo.json`.
Verbatim, cow:

> `body: "origin": [ -6.0, 11.0, -5.0 ], "size": [ 12, 18, 10 ]` with
> `"bind_pose_rotation": [ 90.0, 0.0, 0.0 ]`
> `leg0: "origin": [ -6.0, 0.0, 5.0 ], "size": [ 4, 12, 4 ]`

**Units and axis convention, established rather than assumed.** 16 model units
= 1 block, shown by example at
`https://minecraft.wiki/w/Tutorial:Models` (`"from": [0,0,0], "to": [16,16,16]`
for a full block); the wiki's `Model` page does not state the rule outright,
which is the weakest link in the unit chain — though the ratios below are
unitless so it does not matter to the conclusion. The torso's
`bind_pose_rotation` of 90° about X means a body `size` of `[12, 18, 10]` reads
as width 12, **length** 18, height 10. Cross-checked against the published
hitbox: `https://minecraft.wiki/w/Cow` — *"Height: 1.4 blocks, Width: 0.9
blocks"* → 1.4 × 16 = 22.4 units, against leg 12 + body height 10 = 22.

| mob | leg thickness / leg length |
|---|---|
| cow | 4/12 = 0.333 |
| pig | 4/6 = 0.667 |
| sheep (sheared) | 4/12 = 0.333 |
| horse | 4/11 = 0.364 |
| wolf | 2/8 = 0.250 |
| polar bear | 4/10 = 0.400 |
| llama | 4/14 = 0.286 |
| panda | 6/9 = 0.667 |
| fox | 2/6 = 0.333 |
| goat | 3/10 = 0.300 |
| **mean** | **0.393** |

Body width / body length averages **0.595** over the same ten.

### 2.2 Veloren (GPL voxel RPG)

The Rust source carries bone *offsets* only — `quadruped_medium/mod.rs`
declares `leg_f: (f32, f32, f32)` and friends, which are positions. The
thicknesses were measured from the shipped models at
`https://gitlab.com/veloren/veloren/-/raw/master/assets/voxygen/voxel/npc/<species>/male/<part>.vox`,
parsed as occupied bounding boxes out of the binary SIZE/XYZI chunks.

| species | leg thickness / leg length |
|---|---|
| horse | 4/18 = 0.222 |
| cattle | 5/19 = 0.263 |
| wolf | 4/18 = 0.222 |
| panda | 4/17 = 0.235 |
| bear | 4/17 = 0.235 |
| deer | 3/15 = 0.200 |
| **mean** | **0.230** |

Body width / body length averages **0.505**.

### 2.3 Infinigen (BSD-3, Princeton) — the photoreal reference

`https://github.com/princeton-vl/infinigen`, in
`src/infinigen/assets/objects/creatures/parts/leg.py`. There is no file called
`quadruped.py`; the genomes are `herbivore.py` and `carnivore.py`. Verbatim:

```python
class QuadrupedBackLeg(PartFactory):
    def sample_params(self):
        return {"length_rad1_rad2": np.array((1.8, 0.1, 0.05)) * N(1, (0.2, 0, 0), 3), ...

class QuadrupedFrontLeg(PartFactory):
    def sample_params(self):
        return {"length_rad1_rad2": np.array((1.43, 0.1, 0.1)) * N(1, (0.2, 0, 0), 3), ...
```

A single vector — length, proximal radius, distal radius. Diameter over
length: **back 2×0.1/1.8 = 0.111, front 2×0.1/1.43 = 0.140**, carnivore
2×0.1/1.6 = 0.125.

Two things worth taking from how it is factored, beyond the numbers.
**Thickness tapers and is authored as two radii**, proximal and distal — the
back leg 2:1, the front leg not at all. And the noise term
`N(1, (0.2, 0, 0), 3)` varies the LENGTH at 20% sigma while holding both radii
exactly fixed; the species genomes loosen that only to 5% on the radii. **A
procedural quadruped generator that has been looked at varies limb length far
more than limb thickness.** This library does the opposite — `variation.shape`
moves `quad.depth`, which used to move the limb — and that is worth revisiting
separately.

### 2.4 The readability floor, from shipped art rather than from opinion

No citable numeric guideline for voxel feature readability was obtained; the
intended source (`blog.studiominiboss.com/pixelart`) no longer resolves. What
the two corpora above show empirically:

- Minimum limb cross-section in ten shipping Minecraft quadrupeds is **2
  units** (wolf `[2,8,2]`, fox `[2,6,2]`); the mode is 4. Nothing ships at 1.
- Minimum limb cross-section across 48 measured Veloren quadruped parts is
  **2 voxels** (`deer_foot_fr`, 2×3×8); upper legs are 3–5.
- One-unit features do ship in both, but only as detail that carries no
  weight — Minecraft's cow horn is `"size": [1, 3, 1]`.

**2 voxels is the floor for a limb; 3–4 is where both corpora live.** This is
consistent with the house rule that a feature needs about three voxels across
to read (`forge/kinds.py`), and it is the reason the generator's degeneracy
guard was raised from a half-voxel radius (a 1-voxel limb) to a whole one.

---

## 3. Real anatomy

### 3.1 Girth against withers height — the checkable livestock ratio

All centimetres, confirmed from the headings and methods of each paper.
Ratios are arithmetic on the published means.

| population | withers ht | heart girth | girth/WH | source |
|---|---|---|---|---|
| Tarai cattle ♀ | 104.2 | 132.6 | **1.27** | [PMC11873396](https://pmc.ncbi.nlm.nih.gov/articles/PMC11873396/) |
| Tarai cattle ♂ | 115.1 | 145.7 | **1.27** | same |
| Sika deer ♂ ≥4 yr | 103.12 | 126.50 | **1.23** | [PMC13162871](https://pmc.ncbi.nlm.nih.gov/articles/PMC13162871/) |
| Maremmano horse | 164.31 | 194.11 | **1.18** | [PMC11310958](https://www.ebi.ac.uk/europepmc/webservices/rest/PMC11310958/fullTextXML) |
| Sardinian Anglo-Arab | 160.8 | 185.9 | **1.16** | [PMC7277675](https://pmc.ncbi.nlm.nih.gov/articles/PMC7277675/) |
| Yanqi horse | 132.07 | 150.01 | **1.14** | [PMC13255931](https://pmc.ncbi.nlm.nih.gov/articles/PMC13255931/) |
| Xhosa goat ♀ | 61.56 | 85.08 | **1.38** | [PMC12508844](https://pmc.ncbi.nlm.nih.gov/articles/PMC12508844/) |
| West Shewa goat | 65.97 | 70.87 | **1.07** | [PMC12240294](https://pmc.ncbi.nlm.nih.gov/articles/PMC12240294/) |

The Tarai cattle paper defines the two measurements verbatim: heart girth
*"Place the measuring tape around the animal at the point of the smallest
circumference, just behind the forelegs"*; wither height *"Distance (vertical)
from the bottom of the front foot to the highest point above wither."* The
sika deer paper: BH *"Vertical distance from the highest point of the withers
to the ground (cm)"*, CC *"Chest circumference measured posterior to the
scapula (cm)"*.

**Working range: 1.14 (horses) to 1.38 (goats).**

**Two traps recorded so nobody re-walks into them.** The Yanqi horse table's
dispersion column is headed "SD" but cannot be one — a withers-height SD of
0.96 cm against a printed 98–152 cm range is impossible, so it is standard
error and the derived CV% column is wrong too; the means are usable and the
dispersion is not. And the Xhosa goat paper reports a "body depth" of
71.7–78.4 cm against withers heights of 61.6–69.3 cm, which is anatomically
impossible for a chest depth, so it measures something else and **no chest
depth ratio is quoted in this file at all.**

### 3.2 The fleshed limb

The one source with live tape measurements of a limb, on cattle
([PMC10548415](https://pmc.ncbi.nlm.nih.gov/articles/PMC10548415/)), defines
both stations verbatim — *"Cannon circumference: circumference of the left
forelimb measured using flexible tape at the point of least circumference
distal to the carpal joint"*, *"Forearm circumference: … immediately distal to
the lateral epicondyle of the humerus"* — and gives, at 150 days and 202.8 kg,
cannon 15.8 cm and forearm 39.7 cm. **The muscled forearm is 2.1–2.5× the
girth of the cannon**, so a fleshed foreleg is nowhere near a uniform cylinder.

Cannon circumference over withers height converges hard across four
independent adult populations: Maremmano **0.126**, Yanqi **0.126**, Sardinian
Anglo-Arab **0.127**, sika deer **0.110**.

Putting those together for a 164 cm horse: cannon circumference 20.8 cm →
diameter 6.6 cm; forearm therefore roughly 14–16 cm. Its free foreleg — below
the chest — is on the order of half the withers height, about 82 cm
**[estimate: derived from the withers height, not measured]**. So a real
horse's foreleg runs from **t/L ≈ 0.08 at the cannon to ≈ 0.18 at the
forearm**, averaging near **0.12**.

That is an independent third arrival at the same figure as Infinigen's 0.111 /
0.140 and the owner's own thinnest accepted species at 0.115.

### 3.3 Limb bone circumference against body mass — read, and NOT used

Campione & Evans 2012, *BMC Biology* 10:60
([PMC3403949](https://www.ebi.ac.uk/europepmc/webservices/rest/PMC3403949/fullTextXML)):

> "we propose the following scaling equation as a robust predictor of body mass
> in quadrupedal tetrapods: logBM = 2.749⋅logC(H+F) − 1.104 … (R2 = 0.988, and
> a mean PPE of 25.6%)."

**The paper never states its units.** They were established two ways: the
author's own R package documents `BM` as *"a numeric vector of body masses (in
g)"* and `HC` as *"(in mm)"*
(`https://search.r-project.org/CRAN/refmans/MASSTIMATE/html/extants.html`), and
refitting OLS on the dataset shipped in that package (n = 245, matching the
paper's *"245 (talpids removed)"*) reproduces slope 2.7493 and intercept
−1.1044 to four decimals. **Millimetres and grams, log base 10.** A second
trap: the paper's Table 1 is a standardized major axis fit (m = 2.7779,
b = −1.1564) and is *not* the predictive equation.

Measured specimen values from that dataset, for orientation only:

| species | mass (kg) | humerus + femur circumference (mm) |
|---|---|---|
| *Loxodonta africana* | 6435 | 815.3 |
| *Bison bison* | 1179 | 359 |
| *Alces alces* | 523 | 289.5 |
| *Ursus arctos* | 435.5 | 272.5 |
| *Equus burchelli* | 272.4 | 255.5 |
| *Tragelaphus strepsiceros* | 301 | 275 |
| *Odocoileus virginianus* | 66.1 | 149 |

**REJECTED as a sizing input, with numbers.** A bare humerus circumference of
140 mm on a 301 kg kudu is a bone diameter of 4.5 cm. The kudu's fleshed
foreleg is not 4.5 cm through, and the sika deer study makes the gap explicit:
its *live* cannon circumference (11.34 cm) exceeds its *predicted bare femur*
circumference (~9.1 cm) at the same station. Osteological circumference is a
mass estimator, not a silhouette. It is recorded here because it is the
literature the brief pointed at and because knowing it is the wrong input is
worth as much as the right one.

Anderson, Hall-Martin & Russell 1985, *J. Zool.* 207:53–61 (DOI
10.1111/j.1469-7998.1985.tb04915.x) — **citation confirmed via Crossref, paper
paywalled and not read.** Its equation survives second-hand in Campione's
package as `BM(g) = 0.078 × (HC+FC in mm)^2.73`. No number is taken from it.

### 3.4 Species measurements

From Animal Diversity Web (`https://animaldiversity.org/accounts/<Species>/`),
which prints dual units inline so a percentage-under-a-millimetre-heading trap
cannot arise. Shoulder height / head-body length: American bison ♂ 1.67–1.86 m
/ 3.6–3.8 m total; brown bear 0.90–1.50 m / 1.0–2.8 m head to rump; wild boar
≤0.9 m / 1.53–2.40 m total; gemsbok 1.15–1.25 m / 1.80–1.95 m; greater kudu
1.00–1.50 m (no length published); moose ≤2.3 m / 2.4–3.1 m; plains zebra
1.10–1.45 m / 2.17–2.46 m.

**Three warnings.** ADW's `Cervus_elaphus` page is titled "(elk)" and describes
North American wapiti — those are **not** European red deer numbers, and the
red deer figures used in this library remain **[estimate]**. Plains zebra must
be read from `Equus_burchellii`, not `Equus_quagga`, which is the extinct
quagga. And "length" is inconsistent across pages — "total length", "nose to
tail" and, for bear, explicitly "head to rump".

**Chest girth and limb circumference could not be found for any of the eight
wild species.** Every live limb-girth dataset in the literature is a domestic
production or companion animal, because the measurement exists for breeding
and welfare reasons. Two directly relevant wild papers are blocked (Wallin et
al. 1996 moose chest circumference, Wiley 403; Cook et al. 2003 elk chest
girth, no open copy) and the *Mammalian Species* accounts are image-only scans
behind a 403.

---

## 4. ADOPT / REJECT

### ADOPTED

1. **Limb thickness is a fraction of limb length, not of trunk depth.**
   `quad.leg_thick` re-referenced in `forge/quadruped.py`; all 131 species
   converted by `tools/retune_quad_bulk.py`. §1.2 for why, §2 for the
   reference values it is now quoted in.
2. **A working target of t/L = 0.16.** Above every species the owner accepted
   at the thin end (boar 0.115, warthog 0.136); above a real horse's fleshed
   foreleg averaged over its taper (~0.12, §3.2); below Veloren's shipping
   0.230 and far below Minecraft's 0.393. The failure gate in
   `tools/quadprobe.py` is set lower, at **0.11**, which is simultaneously the
   thinnest accepted species and Infinigen's photoreal back leg.
3. **A two-voxel floor on limb thickness**, from §2.4: neither shipped corpus
   contains a one-voxel load-bearing limb.
4. **A trunk girth floor of 0.95 of withers height**, deliberately short of
   the real 1.14–1.38 — see the rejection below.
5. **Girth measured at mid-barrel, not behind the elbow** (§1.3), and labelled
   as such rather than passed off as a heart girth.

### REJECTED, with the numbers

1. **Limb thickness in voxels as the diagnostic.** Accepted species 3.0–5.0,
   rejected species 3.0–5.0. Complete overlap (§1.1).
2. **Limb thickness over withers height as the diagnostic.** wild-boar 0.055
   (accepted) against gemsbok 0.050 (rejected) (§1.1).
3. **Trunk girth over withers as the diagnostic.** On the corrected
   measurement, accepted 0.88–1.12 against rejected 0.64–0.78 — the boar
   (0.88) and the zebra (0.78) are closer to each other than either is to the
   ends of its own group (§1.3). It is a real deficiency and it is being
   fixed, but it is not what the owner was pointing at.
4. **Taking trunk girth all the way to the live-animal 1.14–1.38.** The owner
   looked at animals measuring 0.88–1.12 on this metric and called four of
   them solid. Going to 1.2 would rebuild species he did not complain about on
   the authority of a livestock tape measure rather than a render. Stopped at
   0.95, which clears the entire rejected group (0.64–0.78).
5. **Long-bone circumference allometry as a sizing input.** A 140 mm humerus
   circumference on a 301 kg kudu is a 4.5 cm bone; the fleshed limb is
   roughly twice that, and the sika deer numbers show live cannon girth
   exceeding predicted bare femur girth at the same station (§3.3).
6. **Minecraft's own ratios, 0.393 and a body 0.595 as wide as it is long.**
   That is a toy, and the brief for this library is a recognisable species at
   a distance. Veloren's 0.230 is the nearer neighbour and even that is 1.9×
   real; 0.16 is a deliberate stop between real and stylised.
7. **A finer lattice as the fix.** 24 species sat at 5 cm and 58 had a foreleg
   under three voxels across, so "move them to 2 cm" is the obvious move and it
   is the wrong one: **a lattice multiplies both sides of a ratio and leaves it
   alone.** Measured, on `red-deer-stag` at its authored 2 cm and one tier
   finer at 1 cm:

   | | foreleg across | free limb | t/L | asset |
   |---|---|---|---|---|
   | 2 cm, as authored | 3 vox | 50 vox | **0.060** | 16,283 vox |
   | 1 cm, one tier finer | 7 vox | 101 vox | **0.069** | 124,085 vox |

   **7.6× the voxels bought 15% of the ratio** — and all of that 15% is
   rasterisation rounding in the finer lattice's favour, not a change in what
   was drawn. The proportion fix, by contrast, took the same animal from 0.060
   to 0.163 at its own 2 cm. **Proportion did essentially all of the work and
   the lattice did essentially none of it.** One species was moved to a finer
   lattice by this change and it was for
   an unrelated reason — `white-tailed-deer` 5 cm → 2 cm, because its skull at
   5 cm was seven voxels across and could not carry an ear, an antler pedicle
   and an eye at once; see `docs/quadruped-notes.md` §9.4.

### OPEN

- **Limb taper.** Infinigen authors a proximal and a distal radius per
  segment and a real foreleg is 2.1–2.5× thicker at the forearm than at the
  cannon (§3.2). This generator draws one radius with fixed multipliers
  (1.25 at the shoulder, 0.9 at the foot). A `quad.leg_taper` row is the
  obvious next thing and is not in this change.
- **What varies between individuals.** Infinigen varies limb length at 20%
  sigma and holds the radii fixed; this library varies `quad.depth` through
  `variation.shape`, which used to move the limb thickness and no longer does.
  Nothing now varies limb proportion at all. Not obviously wrong, but it is a
  decision that was made by accident.
- **Trunk width.** W/L sits at 0.21 across the library against 0.505 in
  Veloren and 0.595 in Minecraft. Deepening the trunk widens it in proportion
  (`quad.width` is width over depth) so this change moves it, but it does not
  address it, and no real-animal number for it could be sourced (§3.1).

---

## 5. What it measured, after

`tools/quadprobe.py --bulk`, variation pinned off, before against after. Full
tables in `out/quad-bulk-ab/bulk-BEFORE.txt` and `bulk-AFTER.txt`.

| | before | after |
|---|---|---|
| foreleg thickness / length, median over 131 | 0.167 | **0.231** |
| species under the 0.11 gate | **48** | **0** |
| species with a foreleg under 3 voxels across | **58** | **0** |
| trunk girth / withers, median | 0.92 | **1.01** |
| trunk width / trunk length, median | 0.21 | 0.26 |
| species failing a bulk gate | 96 | **1** |

The after median of 0.231 lands within 0.001 of Veloren's measured mean
(0.230), which was not aimed at — the target was 0.16 per species and the
median came out higher because the small and low-slung species are naturally
above it.

**Twenty species did not move at all**, and that is the "do not make the good
ones worse" property holding by construction rather than by inspection: they
were already above both floors, and both passes only ever lift. They include
three of the four the owner called solid (`american-bison`, `brown-bear`, and
— on limb thickness — `warthog`), plus `western-lowland-gorilla`,
`red-kangaroo`, `wild-yak`, `european-badger` and `north-american-porcupine`.

**Fourteen species show a LOWER girth/withers afterwards and none of them got
thinner.** They are the low-slung ones — seven lizards, the mole, the
hedgehog, the hippopotamus, the pika. Their trunks are untouched and their
absolute girth is identical (`sand-lizard` 11.1 voxels before and after); what
changed is the denominator, because a thicker leg stands the animal higher.
`sand-lizard`'s withers went 6 → 7 voxels. Worth knowing before reading that
column as a regression.

### The one that did not make it

`maned-wolf` finishes at 0.83 of the girth floor with `quad.depth` at 0.90,
which is the row's ceiling. It is not a silent clamp — `tools/retune_quad_bulk.py`
prints `STILL SHORT` for it and `--bulk` fails it — and it is arguably correct
anatomy: the animal stands at 0.95 of its own head-body length at the shoulder,
which is the most extreme in the library, and a trunk deeper than 0.90 of its
own length is not a mammal. Its limbs are fine (0.206). Left as a reported
outcome rather than forced.

### Two defects this work exposed rather than caused

Both were found by `tools/quadprobe.py --parts` and neither renders wrong.

1. **`okapi`'s horns had no joint.** An ear roots within 0.2 of a head radius
   of a horn's pedicle, so on a species with a big ear the ear plate covers the
   pedicle: the two 23-voxel spikes touched ear on 70 faces and skull on none.
   Fixed by giving the horn a pedicle run down into the skull, in `_headgear`.
2. **`white-tailed-deer`'s right ear had no joint**, and had shipped that way.
   At 5 cm its skull is seven voxels across, its antler beam is drawn through
   where the ear sits, and the ear survived as two voxels touching only antler.
   The left ear was fine, which is what a sub-voxel rounding difference looks
   like. Fixed by the lattice, 5 cm → 2 cm.

And one this work did cause, caught by `--stance`:

3. **A thick limb can be drawn through the floor.** A hock is a centreline
   point and `grid.capsule` sweeps a ball of the limb's radius along it, so a
   joint closer to the ground than its own radius goes under it. The
   hippopotamus's hock sat 2.6 voxels up on a 4.6-voxel radius and reached 1.5
   voxels below the ground plane; the crop then moved the asset's floor and the
   probe reported its FORE feet 2 voxels in the air. Fixed by `_clear` in
   `_legs`, which lifts a mid-limb joint to at least its own radius.
