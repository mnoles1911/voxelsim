# Fitting land-animal proportions to measured references

Companion to `tools/reffit.py`, `tools/refsil.py`, `tools/refnames.py`,
`tools/refsheet.py` and `refs/`. Written 2026-08-15, after the owner asked
whether open-source photographs of each species — left, right, front, rear, top
— could drive generation instead of judgement.

**The gap being closed.** `docs/quadruped-proportion-research.md` §4 records,
honestly, that the limb target of **0.16** is a judgement call sitting between
photoreal (Infinigen 0.111–0.140), a shipping voxel RPG (Veloren 0.230) and a
toy (Minecraft 0.393). That one constant was then applied to 131 species.
`docs/biomes/README.md` §8 says the same of every species size in the lists.
This file is about replacing one of those constants with a per-species
measurement.

**Citation rule, inherited.** This project has shipped a fabricated citation
once (an orca eye patch "measured" from what were dimensionless indices) and a
second agent found a morphometrics table printing percentages under headings
that read like millimetres. So every number below carries the URL it was read
from; anything recalled rather than read is marked **[estimate]**; and where a
source's units had to be established rather than read off, the method is given.

**Licence rule, inherited.** This library has already refused two datasets on
licence grounds — FishBase is non-commercial, FishShapes contradicts itself
between CC0 and CC BY-NC. So a licence here is recorded **per file**, read off
the artifact, never inferred from the site it was found on.

---

## 1. What was built, in one paragraph

`tools/reffit.py` downloads licence-clean scientific silhouettes (PhyloPic,
CC0), reduces them to dimensionless proportions, measures **our own asset the
same way through a true orthographic projection of its voxel grid**, and solves
`quad.leg_thick` until the two agree. The reference is a **judge**, not a source
of geometry: the generator, the part tags, the joints, the sexes and the seeds
are all untouched. Everything a build needs is checked in under `refs/`; `fetch`
is the only command that touches a network, and nothing under `forge/` imports
any of it.

---

## 2. Sources, with licences read per file

### 2.1 ADOPTED — PhyloPic (the pipeline is built on this)

`https://api.phylopic.org/` — a working JSON API, HAL-style, reachable directly.
Licence is **per image**, at `_links.license.href`. For
`81caf94e-5cbe-4e5e-8101-545abea2bfc0` (*Equus quagga*), read verbatim from
`https://api.phylopic.org/images/81caf94e-5cbe-4e5e-8101-545abea2bfc0?build=549`:

```json
"license": { "href": "https://creativecommons.org/publicdomain/zero/1.0/" }
```

`tools/reffit.py` accepts **CC0 1.0, Public Domain Mark 1.0 and CC BY only**,
and refuses NonCommercial, ShareAlike and NoDerivatives at download — SA because
it would reach into the output, ND because it forbids the very act of reducing a
picture to a measurement. Across the library's 131 species it kept **321 files
for 102 species** (CC0 249, CC BY 4.0 32, CC BY 3.0 22, Public Domain Mark 18)
and **refused 36 on licence** (CC BY-NC 3.0 ×20, CC BY-NC-SA 3.0 ×11, CC BY-SA
3.0 ×5). The refused files are not on disk. Attribution for what was
kept is discharged in `refs/silhouettes/ATTRIBUTION.md`.

Two gotchas, both found the hard way and both now handled in code:

- `https://api.phylopic.org/` answers **307 with an empty body**. Without
  `curl -L` the first call of a fetch returns zero bytes and the JSON decoder
  raises "line 1 column 1", which reads like a corrupt API and is a missing flag.
- `filter_name` is **case-sensitive and lowercase**. `filter_name=Equus%20quagga`
  returns `"totalItems":0`; `equus+quagga` returns results. It fails **silently
  with a valid empty response**, not with an error.

### 2.2 ADOPTED as literature — Infinigen (BSD-3-Clause, Princeton)

Licence read in full at
`https://raw.githubusercontent.com/princeton-vl/infinigen/main/LICENSE`:
*"BSD 3-Clause License / Copyright (c) 2023, Princeton University"*. No
non-commercial clause, no field-of-use restriction. Obligations are the
copyright notice and not using Princeton's name to endorse asset-forge.

**The paths in our existing docs have moved** — creature assets are under
`src/infinigen/...` now, and the three paths cited in
`docs/quadruped-proportion-research.md` §2.3 all 404. Working path:
`.../main/src/infinigen/assets/objects/creatures/parts/leg.py`. The numbers
there are unchanged: back leg `(1.8, 0.1, 0.05)` → 0.111, front leg
`(1.43, 0.1, 0.1)` → 0.140.

Three things worth having that were not in our docs before:

- **Front leg / back leg = 1.43 / 1.8 = 0.794.**
- Elbow sits at **0.5 of the back leg** and **0.6 of the front leg**.
- **The quadruped body is not numeric.** `herbivore.py` and `carnivore.py` do
  not call `QuadrupedBody`; they call `NurbsBody`, which loads artist-authored
  `.npy` control points (`body_herbivore_cow.npy`, `body_feline_tiger.npy`, …).
  So `(1.7, 0.65, 0.65)` in `body.py` is a part default, **not** "Infinigen's
  quadruped body", and must not be cited as one.
- **`Neck` is dead code in both genomes.** In `carnivore.py` the neck lines are
  commented out and the head attaches directly to the body at t≈0.97. Infinigen
  offers no shipped precedent for an explicit neck bone.

### 2.3 REJECTED — SMAL / SMALR / SMALST, on licence *and* on provenance

Licence read in full at
`https://raw.githubusercontent.com/silviazuffi/smalst/master/LICENSE_SMAL_MODEL.txt`.
Title line verbatim: *"Software Copyright License for non-commercial scientific
research purposes"*. The decisive clause, verbatim:

> "Any other use, in particular any use for commercial purposes, is prohibited.
> This includes, without limitation, incorporation in a commercial product, use
> in a commercial service, or **production of other artifacts for commercial
> purposes**."

That last phrase names this use case exactly. `docs/marine-megafauna-research.md`
already rejected SMAL on the same grounds; this is a second, independent reading
of the same licence and it agrees.

**A trap worth recording: the GitHub badge says MIT.**
`https://raw.githubusercontent.com/silviazuffi/smalst/master/LICENSE.txt` is a
plain MIT licence covering Zuffi's *code*. The *model* is under the separate
non-commercial file above. Anyone reading the sidebar alone would conclude,
wrongly, that SMAL is MIT.

**And a second reason to reject it that has nothing to do with licence.** From
the CVPR 2017 paper (`https://files.is.tue.mpg.de/black/papers/smal_cvpr_2017.pdf`,
text extracted locally), verbatim:

> "Since scanning live animals is impractical we instead scan realistic toy
> animals to create a dataset of 41 scans of a range of quadrupeds"

> "We estimated a scaling factor so animals from different manufacturers were
> comparable in size."

**SMAL's shape statistics come from 41 toy figurines with an estimated
inter-manufacturer scale factor.** Even licensed, it is not an anatomical
measurement source, and this library would have been fitting itself to somebody
else's toys. The paper publishes **no** limb proportions in real units at all —
the only numbers in it are registration errors — so there is nothing citable to
take from it even second-hand.

**Citation correction.** `10.1109/CVPR.2017.322` is **not** SMAL: Crossref
returns *"BIND: Binary Integrated Net Descriptors for Texture-Less Object
Recognition"*. SMAL is **`10.1109/CVPR.2017.586`**, *"3D Menagerie: Modeling the
3D Shape and Pose of Animals"*, pages 5524–5532. The wrong DOI resolves cleanly
to a real 2017 CVPR paper, so a spot-check that only confirms "the DOI exists"
passes. It is not in this repository and this note is to keep it out.

### 2.4 REJECTED — MorphoSource, with the numbers

API is open and needs no key: `https://www.morphosource.org/api/media?q=Equus`
returns JSON. There **is** a machine-readable licence field and it is usually
empty. Verbatim from one record:

```json
"license":[], "permits_commercial_use":["CommercialUseNotPermitted"],
"copyright_statement":["http://rightsstatements.org/vocab/InC-NC/1.0/"]
```

Licence facet counts summed over ten queries (queries overlap, so these are
proportions and not counts): CC BY-NC **1026**, CC BY-NC-SA **394**, licence
unknown **215**, CC BY-NC-ND **191**, **CC BY 23**, CC BY-SA 5, CC BY-ND 2.

**Commercially usable CC licences are about 1.6% of the records that carry any
CC licence at all**, and most records carry none. Rights statements are
dominated by "In Copyright" (9061) and "Undetermined" (3847).

**And even the usable 1.6% would not help**, which is the more interesting
finding: MorphoSource publishes **no dimensions**. The fields are
`x_pixel_spacing`, `y_pixel_spacing`, `unit` — *scanner voxel resolution*, not
anatomy. To get a limb length you must download and measure the mesh, and the
mesh is the encumbered artifact. The brief's hope that "skeletons give limb
lengths directly" does not survive contact with the repository: they give
**meshes**, and the measuring is left to you.

Its `/about/terms` page is behind bot protection and returned a challenge to
both WebFetch and curl, so **the current terms of use were not read** — the
conclusion above rests on the API's own rights fields plus a 2020 archived
snapshot of the legacy site.

### 2.5 REJECTED — Digimorph

`http://digimorph.org/aboutdigimorph.phtml`, verbatim:

> "Any commercial reproduction, redistribution, publication, or other use of the
> website content, by electronic means or otherwise, is prohibited unless
> pursuant to a written agreement signed by the copyright holder."

No dimensional data is published — animations, STL files and bibliography only.

### 2.6 Keypoint datasets: one adopt, three rejects, and a trap

The brief's hypothesis was that **keypoints ARE joints**, and so might be a
cheaper route to proportion than silhouettes. Tested, and it is half right.

| dataset | licence (read) | keypoints | verdict |
|---|---|---|---|
| **AP-10K** | **CC BY 4.0**, full text in repo `LICENSE`; README: *"The dataset follows CC-BY-4.0 license."* | **17, including shoulder AND hip** | **the only one that could give limb segments** |
| StanfordExtra | MIT, `LICENSE` read | 24, but **no shoulder, no hip** | annotations usable; cannot give a proximal segment |
| Animal-Pose | **none — no LICENSE file, GitHub `license` field `null`** | 20, no shoulder/hip | REJECT: no licence is no permission |
| Animal3D | repo MIT but **contains no data**; data inherits PartImageNet/COCO | 26 (names never enumerated in the paper) | REJECT: data licence unresolved |

**REJECTED — StanfordExtra and Animal-Pose as proportion sources, with the
reason.** Both stop at the elbow and the knee. StanfordExtra's most proximal
limb point is literally named `top` and maps to `L_F_Elbow`; Animal-Pose has
*"Four Elbows, Four Knees, Four Paws"* and no shoulder or hip. **You cannot
measure a limb's length without knowing where it starts**, so these give the
distal two segments and nothing else.

**AP-10K carries a naming trap that would have produced exactly this project's
signature error.** Verbatim from its README:

> "For simplicity, we use 'hip' to denote the knees for unguligrade and
> digitigrade animals and 'knee' for their ankles. For plantigrade animals, the
> annotation is the same as the biology definition."

So for a horse or a dog, the point labelled **"hip" is the knee** and the point
labelled **"knee" is the ankle**. Deriving a femur length from AP-10K across
species would be anatomically wrong for the large majority of its 54 species —
the same class of error as reading dimensionless indices as millimetres.
AP-10K is **not used here**; it is recorded as the best remaining lead, with its
trap written down before anyone walks into it.

### 2.7 Not used, and why — Wikimedia Commons, BHL, GBIF

**Wikimedia Commons works** and is the obvious next source. The API returns file
list, download URL and a complete per-file licence record in **one call**:

```
https://commons.wikimedia.org/w/api.php?action=query&format=json
  &generator=categorymembers&gcmtitle=Category:Side%20views%20of%20tigers
  &gcmtype=file&gcmlimit=3&prop=imageinfo&iiprop=url|size|extmetadata
```

`extmetadata` carries `LicenseShortName`, `UsageTerms`, `LicenseUrl`, `Artist`,
`AttributionRequired` and **`Restrictions`** — the last being where
non-copyright restrictions surface, and a non-empty value must be a hard stop.

The owner is right that near-orthographic sources are common, and Commons proves
it institutionally: `list=allcategories&acprefix=Side%20views%20of%20` enumerates
**478 categories**, of which ~31 are mammals — `Side views of gray wolves` 568
files, `Side views of horses` 246, `Side views of tigers` 194, `Side views of
bears` 20. There is also `Category:Mammalia in right lateral aspect`.

It is **not used in this pass** for two honest reasons: coverage is uneven and
skewed to carnivores (**no zebra and no deer side-view category exists**), and
these are human-curated judgements of "roughly lateral" on ordinary perspective
photographs — so a silhouette extracted from one needs segmentation this
environment has no library for (`scikit-image` is absent). PhyloPic gives a
clean binary mask for free. Commons is the right second source and the first
thing to build next.

`Category:Lateral_views` **404s** and `incategory:"Lateral views of animals"`
returns zero hits; those names were guesses and are wrong.

**REJECTED — Biodiversity Heritage Library.** Requires an API key (`api3`
returns **401**); `www.biodiversitylibrary.org/copyright`, `/help/copyright` and
`/docs/api3.html` all return **403** to automated fetches. Rights position read
from the one reachable host, verbatim: in-copyright material is *"Almost always
a Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International"* with
*"No commercial use"*, while public-domain status is a US-only 95-year date rule
that BHL explicitly declines to stand behind. Mixed NC corpus plus a disclaimed
PD determination plus a key requirement — not licence-clean by default.

**GBIF is used only to resolve names, never for images**, and there is a trap
worth recording. `...&license=CC0_1_0` filters the **occurrence record**, not
the **photograph**: a query so filtered returned three records whose media
licences were CC0, **CC BY-NC 4.0** and **CC BY 4.0**. Any GBIF-based image
pipeline must filter on `media[].license` client-side, after fetching.

### 2.8 Literature — what real animals measure

Used as a cross-check on the silhouettes, not as a fitting target.

| quantity | value | source | how the units were established |
|---|---|---|---|
| chest girth ÷ withers height, horse (n=50) | **1.07–1.14** | [PMC11462137](https://europepmc.org/article/PMC/PMC11462137) | row labels carry units inline (`HW (cm)`, `GC (cm)`); the ratio column `GCHW` carries none **because it is a ratio**, and 157.44/141.83 = 1.110 reproduces it in all four groups |
| red deer limb chain, *Cervus elaphus*, n=1 | radius 266.9, mc 231.6, femur 278.0, tibia 329.0, mt 265.1 mm | [PMC6227981](https://europepmc.org/article/PMC/PMC6227981) | a spanning header `bone length (mm)` sits over the columns |
| humerus + femur, 282 taxa | zebra 259.0 / 363.5; grizzly 400.5 / 445.5; wolf 211.0 / 226.5 mm | [PMC3403949](https://europepmc.org/article/MED/22781121) suppl. `1741-7007-10-60-S1.XLS` | **no units in the headings**; established from Methods (*"Length measurements less than 150 mm were taken with digital callipers…"*) and cross-checked against the paper above, which labels its units: *Alces alces* femur 440.0 there vs 445.9 here |
| head ÷ body, sika deer | 0.291 | [PMC12897076](https://europepmc.org/article/PMC/PMC12897076) | units inline in each row label (`Head Length, cm`) |

**Three traps found in this literature and not walked into.**

1. **The sambar deer paper prints `Thoracic diameter (cm)` = 100.8 on an 89.6 cm
   withers height** ([PMC12413589](https://europepmc.org/article/PMC/PMC12413589)).
   A diameter cannot exceed the animal's height; it can only be a
   circumference. Read as girth it gives 1.125, which lands neatly on the
   horse's 1.07–1.14 — and that agreement rests on **reinterpreting a heading**,
   not on what the paper printed. **No number is taken from it.**
2. **The sika deer chest-circumference column is not trustworthy**: CV 28.4%,
   minimum 57 cm against a chest *depth* of 39–54 cm. A 39 cm-deep chest cannot
   have a 57 cm circumference.
3. **ADW's grey wolf entry does not close.** Nose-to-tail 1000–1300 mm minus
   tail 350–520 mm leaves a head-body of 480–950 mm against a stated shoulder
   height of 600–900 mm — a wolf as tall as it is long. No shoulder ratio is
   taken from that page.

`Bison bison`'s row in the 282-taxon dataset has **both bone-length cells
blank** — only circumferences. The dataset that should have settled bison limb
proportions does not contain them.

---

## 3. The measurement, and the four ways it was wrong first

`tools/refsil.py` reduces a side silhouette to dimensionless proportions. The
same function runs on a PhyloPic raster and on a true orthographic projection of
our own voxel grid (`data.any(axis=1)` — not a render; `broadside` tilts a few
degrees up, which lifts the far flank into the silhouette and thickens every
limb).

### 3.1 The validation that matters

Silhouette measurement against `quadprobe.m_limb_slender` on **our own assets**,
seed 1, variation pinned off:

| species | quadprobe | silhouette | error |
|---|---|---|---|
| american-bison | 0.250 | 0.207 | −17.0% |
| plains-zebra | 0.156 | 0.136 | −12.4% |
| red-deer-stag | 0.163 | 0.169 | +3.5% |
| wild-boar | 0.173 | 0.164 | −5.4% |
| greater-kudu | 0.196 | 0.173 | −11.5% |
| grey-wolf | 0.192 | 0.147 | −23.7% |

Mean absolute error 15.6%, **median bias −16.1%**.

**The limb thickness itself agrees exactly** — 5.0 against 5.0 on the bison, 7.0
against 7.0 on the zebra, 9.0 against 9.0 on the kudu. The whole disagreement is
in the denominator, and it is a **definition difference**: `quadprobe` measures
the free limb from its part tag and stops where the leg enters the body; a
silhouette has no part tags and measures down from where the legs visibly
separate.

**So the bias is real and it cancels**, because the reference is measured by the
same code. Nothing in `reffit.py` corrects for it and nothing should — a
correction factor would be a fudge standing where a definition belongs.
`quadprobe.py` keeps its own numbers and its own gates and this does not argue
with it.

### 3.2 Four defects, each caught by looking rather than by a test

1. **The belly line was found by coverage, and a brown bear broke it.** A bear's
   legs are so thick that at ankle height they already cover half the trunk's
   columns, so the belly landed one pixel off the ground and the tool reported
   **a limb four times as thick as it was long**. It did not crash and it did
   not look absurd in a table of ratios; it produced a number. The belly is a
   **topology** change — two runs below, one above — and counting runs does not
   care how thick the legs are.

2. **A horizontal cut across a slanted leg is not its thickness.** Half the
   corpus is drawn mid-stride. The chord through a limb leaning at θ is
   `diameter / cos θ`, and a walking wolverine measured **0.457** against 0.235
   for the same animal standing. Each leg is now tracked up through the band,
   the slope of its centre is fitted, and the width is projected back onto the
   perpendicular. Our own assets stand square, so their slope is ≈0 and the
   correction moves them by 0.1% — which is the test that it fixes the reference
   side and is not a thumb on the scale for both. It also **tightened
   cross-silhouette agreement**, which is independent evidence it removes a real
   artifact: `wild-boar` and `white-tailed-deer` both fell below the spread gate.

3. **Slivers were being counted as legs.** Without a span filter, the
   thickest-over-thinnest "leg" *within one silhouette* reached **149** on a
   hippopotamus and **110** on a pine marten, because a twenty-pixel tail tip was
   averaged against a real limb. Our own assets measure **1.0–1.4** on that
   ratio, which is what two real limbs look like. A leg must now span 60% of the
   measured band.

4. **GBIF's common-name search returns confident garbage.** Unconstrained,
   "brown bear" resolves to ***Protea speciosa***, a shrub, and "red deer" to
   **"Red deerpox virus"**. Constrained to Mammalia it is right most of the time
   — and *most* is not enough when the output selects which pictures a species
   is fitted against. It gave `lion` → ***Macaca silenus***, `nile-monitor` →
   ***Mungos mungo***, `spectacled-caiman` → ***Cebus imitator***. **The mongoose
   reference passed every quality test this pipeline has**: two silhouettes, both
   measurable, in close agreement, promoted to "fit-quality". Nothing downstream
   can tell a good measurement of the wrong animal from a good measurement of the
   right one. `tools/refnames.py` now verifies every binomial against GBIF's own
   vernacular list, and `fit` refuses to move a spec whose name is not
   hand-checked.

   The Mammalia constraint that fixes the shrub also **guarantees a wrong answer
   for every reptile in the library**, which is why `refnames.py` carries a class
   per entry.

   *And the checker had the same class of bug in itself*: its word-overlap test
   required words longer than three letters, so `red-fox` → *Vulpes vulpes*
   contributed no words at all and was reported REJECTED. Correct mapping, wrong
   verdict, from the tool written to catch wrong verdicts.

### 3.2b Is it a proportion? A scale-robustness check

A dimensionless ratio should not care how many pixels the picture has. Measured
on 91 silhouettes at their native 1536 px and again downscaled to 512 px:

| | limb thickness / limb length |
|---|---|
| median change | **+0.29%** |
| mean absolute change | **3.31%** |
| 90th percentile absolute | 8.08% |
| worst | 35.61% |

Typical stability of ~3% is comfortably inside the spread between silhouettes of
the same species, so the quantity is behaving like a proportion. The tail is not
negligible, though — a handful of species move by tens of per cent when
rasterised smaller, which is the voxel-quantisation effect
`docs/quadruped-proportion-research.md` §4.7 already measured from the other
direction. **So the files checked in are the ones that were measured**, at 1536
px and 15 MB for 321 files, rather than downscaled to save space after the fact.

### 3.3 The failure that could NOT be automated, stated plainly

**The gate cannot detect a sitting animal, and four discriminators were built
and measured before accepting that.** A sitting mustelid has a belly line, two
runs across the leg band, a plausible height/length and a plausible thickness —
because the haunch it is sitting on measures exactly like a very thick leg.

| discriminator tried | why it failed, with numbers |
|---|---|
| limb thickness ÷ **body** length | sitting `fisher` 0.159 sits inside the range of real stocky animals; our own standing `pika` measures 0.182 |
| ground-contact width | `fisher` does not reach the top 14; elephants and rhinos rank above it because they genuinely have big feet |
| cross-silhouette agreement | `fisher`'s two references **agree with each other to 21%** — both artists drew the same pose, so no sample size or spread gate ever catches it |
| within-silhouette leg disagreement | 2.48 and 1.00, against a corpus median 1.66 and 75th percentile 2.41. No threshold separates it |

So the defence is **not a cleverer gate**. It is two things, both deliberate:

- `refs/excluded.json` — seven silhouettes refused **by hand after looking at the
  overlay**, each with its reason recorded.
- `MAX_LIFT = 1.5` in `tools/reffit.py` — no single pass may thicken a species
  by more than half again, whatever the reference says, and capped species are
  **printed as capped** so they get a render and a human eye rather than a table
  row.

`tools/reffit.py overlay` draws the belly line and the measured band onto every
silhouette, because **a ratio cannot be eyeballed** and every wrong number this
pipeline produced looked perfectly reasonable in a table.

---

## 4. How far the library was from the references

Reference and asset measured by the **same** code, variation off, seed 1. 80
species have both a spec and a usable reference. `out/reffit/report-BEFORE.txt`,
recomputed against the final reference set so that nothing below is confounded
by the corpus having changed mid-exercise.

| ratio | median, ours ÷ reference | range | within 10% | mean abs error |
|---|---|---|---|---|
| limb thickness / limb length | **1.04×** | 0.22–2.49× | 11 of 80 | **37%** |
| **belly clearance / body length** | **1.26×** | 0.41–2.36× | 19 of 80 | **33%** |
| total height / body length | **1.11×** | 0.38–1.99× | 18 of 80 | 27% |

**The answer to the brief's question is: neither 10% nor 2×, and the two ratios
say different things.**

**Limb thickness — the level is right and the SPREAD is wrong.** A median of
1.04× says the 0.16 judgement call landed close to life on average — but the
**mean absolute error is 37%**, and those two facts together are the finding:
the errors are large and they cancel. Real animals measured off these
silhouettes span **0.083 (roe deer) to 0.54 (brown bear)**, a spread of 6.5 to
1, and a single constant applied to 131 species cannot reproduce that. Only 11
of 80 species were within 10% of their own reference. **The defect is not the
value of the constant; it is that it is a constant.**

**Belly clearance — a systematic, one-directional gap that nothing had
measured.** The library stands **26% too high off the ground**, and this is the
other half of the owner's original complaint. His words were *"tall lanky with
narrow legs and bodies"*; this morning's work fixed **narrow**. Nothing measured
**tall**. The species furthest out are the short-legged and the long-legged
extremes — `stoat` 2.36×, `red-river-hog` 2.10×, `cheetah` 1.85×, `wolverine`
1.81×, `american-black-bear` 1.80×, `moose` 1.78× — which is the same shape of
failure as the original limb bug: **the middle of the distribution is fine and
the extremes are wrong**, because a parameter authored by judgement regresses
toward a generic quadruped.

`plains-zebra`, the best-sampled species (7 usable silhouettes) and one of the
best-authored specs, measures 0.48 against a reference 0.46 — 4% out. So the
measurement is not biased; the library genuinely mis-serves its short-legged
species.

---

## 5. What was re-fitted, and what it moved

`tools/reffit.py fit` solves `quad.leg_thick` per species. **It never reduces**
— see §7 rejection 1 — so it only ever lifts a species that is thinner than
life. 14 of 27 eligible species moved. Full table in
`out/reffit/fit-APPLIED.txt`; renders in `out/reffit/sheets/`.

**Library-wide, over the same 80 species and the same reference values:**

| | before | after |
|---|---|---|
| limb thickness ÷ reference, median | 1.04× | 1.04× |
| species within 10% of their own reference | 11 of 80 | **18 of 80** |
| species within 25% | 37 of 80 | **42 of 80** |
| mean absolute error against life | 37% | **34%** |
| `quadprobe --bulk` thickness/length, median over 131 | 0.231 | **0.250** |
| species failing a `--bulk` gate | 1 | **1** |

The library-wide median does not move, and that is the honest shape of this
result: **only 14 of 80 species had a verified enough reference to fit against**,
so 66 of them are untouched and they dominate the median. The change is real and
it is narrow.

**On the 14 species that were actually re-fitted:**

| | before | after |
|---|---|---|
| median ours ÷ reference | 0.72× | **0.98×** |
| mean absolute error against life | **26%** | **9%** |

| species | t/L before | after | life | before | after |
|---|---|---|---|---|---|
| brown-bear | 0.290 | 0.427 | 0.428 | 0.68× | **1.00×** |
| lion | 0.158 | 0.209 | 0.208 | 0.76× | **1.00×** |
| bobcat | 0.173 | 0.254 | 0.264 | 0.66× | **0.96×** |
| spotted-hyena | 0.170 | 0.204 | 0.209 | 0.81× | **0.97×** |
| striped-hyena | 0.166 | 0.177 | 0.178 | 0.93× | **0.99×** |
| eurasian-lynx | 0.184 | 0.263 | 0.309 | 0.59× | 0.85× *(capped)* |
| cheetah | 0.131 | 0.203 | 0.240 | 0.55× | 0.85× *(capped)* |
| american-black-bear | 0.221 | 0.329 | 0.385 | 0.57× | 0.85× *(capped)* |
| caracal | 0.154 | 0.217 | 0.291 | 0.53× | 0.75× *(capped)* |
| wolverine | 0.166 | 0.219 | 0.340 | 0.49× | 0.64× *(capped)* |
| american-bison | 0.207 | 0.229 | 0.222 | 0.93× | 1.03× |
| wood-bison | 0.208 | 0.228 | 0.222 | 0.94× | 1.03× |
| lowland-tapir | 0.198 | 0.230 | 0.220 | 0.90× | 1.04× |
| warthog | 0.192 | 0.204 | 0.198 | 0.97× | 1.03× |

Six species hit the 1.5× cap and are flagged for a render rather than trusted:
`american-black-bear`, `bobcat`, `caracal`, `cheetah`, `eurasian-lynx`,
`wolverine`.

**Resolved 2026-08-15, one at a time and by looking.** Re-run after the stance
fix, five of the six no longer need the cap at all: `american-black-bear` (0.470
against a reference 0.385) and `eurasian-lynx` (0.320 against 0.309) are now
above life without being touched, and `bobcat` (0.262 vs 0.264), `caracal` (0.310
vs 0.291) and `cheetah` (0.235 vs 0.240) reached their references inside one
pass. **So the cap was not the constraint on those five; the too-long limb it was
being divided by was.**

`wolverine` is the sixth and it is **the reference that is wrong, not the spec**.
Both of its silhouettes are of a walking plantigrade mustelid, and the overlays
show why the number is inflated: the 0.30–0.75 band cuts the legs where the near
and far limbs and the large flat feet are merged into one run, so what is
measured as a limb diameter is a limb plus a paw plus its opposite number. The
cap was protecting the spec here, not holding it back. The files are left in
place — `fit` never reduces, so no harm follows from a reference that reads high,
and the same silhouettes give a perfectly good belly line — but nothing should
chase 0.340 on a wolverine.

**One regression was caused and fixed, and it is the coupling the previous
work documented.** A thicker limb stands the animal higher, and withers height
is the denominator of the girth ratio — so `eurasian-lynx` fell to 0.90 of the
girth floor and `--bulk` failed it. `docs/quadruped-proportion-research.md`
predicts exactly this (*"dingo, giant-anteater and maned-wolf all did exactly
that"*) and `tools/retune_quad_bulk.py --trunk` is the sanctioned remedy; run on
the 14 touched species it deepened one trunk, 0.58 → 0.60, and the probe returned
to its baseline single failure (`maned-wolf`, pre-existing).

**One species crossed below the 0.95 retune *target* — `caracal`, 0.98 → 0.94.**
That is not a gate failure and it is not silent: it is reported here and by the
probe's own "girth below the retune target" line, which went from 37 to 38 of 131.

---

## 6. REJECTED — the visual hull, which is what the brief literally asked for

Five silhouettes — left, right, front, rear, top — intersect into a visual hull,
and that is the obvious reading of "use photographs to inform generation". It is
**not built, deliberately**, and the reasons are structural rather than
aesthetic:

1. **It produces one static mesh.** No part tags, so no rig. This library's land
   animals were given four separately-tagged legs, joint balls that belong to
   their parent, ears, horn pedicles and a solved stance **this week**;
   `quadprobe --parts` and `--caps` exist to defend exactly that. A hull throws
   all of it away.
2. **No sexes, no seeds, no variation.** A hull is one animal. The generator
   draws a stag and a hind from two means, and a herd of forty zebra that are
   not identical.
3. **It cannot be authored against.** A designer can read "a giraffe is a third
   neck" and set `neck_frac`. Nobody can hand-edit a hull.
4. **The reference corpus does not support it anyway.** PhyloPic is lateral
   only; there is no front, rear or top view to intersect. Commons has 31 mammal
   *side*-view categories and no comparable front or top taxonomy.

The reference is used as a **judge** instead: it measures, it does not build.
That composes with everything already in `forge/quadruped.py` and it is the
reason this change is 14 spec numbers rather than a new asset type.

---

## 7. ADOPT / REJECT

### ADOPTED

1. **PhyloPic CC0 silhouettes as the reference corpus.** Per-image licence read
   from the API, 36 files refused on licence, attribution discharged in a file.
2. **Reference and asset measured by the same code**, so the −16% definition
   bias against `quadprobe` cancels instead of needing a correction factor.
3. **A per-species limb target replacing the constant 0.16**, for the 27 species
   with a hand-checked name and a corroborated reference.
4. **A hand-checked scientific name as a hard gate on fitting.** A verified
   mongoose is still a mongoose.
5. **`MAX_LIFT = 1.5` and hand curation** as the defence against bad references,
   *because a geometric gate for sitting poses was attempted and does not exist*.
6. **Infinigen as literature**, BSD-3, with its moved paths and the finding that
   its quadruped body is NURBS data rather than numbers.

### REJECTED, with the numbers

1. **Fitting limb thickness DOWNWARD to life.** Taken literally the references
   say our `red-deer-stag` is **41% too thick** and should fall from 0.169 to
   0.120, and `roe-deer` from 0.152 to 0.083. Those are real measurements of real
   animals — and they are the exact species the owner looked at, called
   wireframes, and had rebuilt this morning. `docs/quadruped-proportion-research.md`
   §4 already settled that this library sits deliberately between photoreal and
   stylised. A photoreal target would undo a fix the owner asked for on the
   authority of a silhouette rather than a render. **The reference is used in one
   direction only.**
2. **SMAL / SMALR / SMALST.** Non-commercial licence naming *"production of
   other artifacts for commercial purposes"*; and independently, shape statistics
   from **41 toy figurines** with an estimated scale factor.
3. **MorphoSource.** ~1.6% of CC-licensed records are commercially usable, and
   the repository publishes **no dimensions** — only meshes, with `pixel_spacing`
   fields that are scanner resolution and not anatomy.
4. **Digimorph.** Commercial use prohibited absent a signed agreement; no
   dimensional data published.
5. **Animal-Pose.** No licence file, GitHub `license` field `null`.
6. **Animal3D.** MIT repo containing no data; image rights inherit from
   PartImageNet and COCO and are unresolved.
7. **StanfordExtra and Animal-Pose as proportion sources.** Both stop at elbow
   and knee — no shoulder, no hip. A limb's length needs to know where it starts.
8. **AP-10K in this pass**, despite being CC BY 4.0 and the only set with
   shoulder and hip: its "hip" means *knee* and its "knee" means *ankle* for all
   unguligrade and digitigrade species, i.e. most of its 54.
9. **BHL.** API key required (401), rights pages 403, corpus mixes public domain
   with CC BY-NC-SA, and PD status is a US-only date rule BHL disclaims.
10. **GBIF as an image source.** Its `license=` filter constrains the occurrence
    record, not the photograph — a CC0-filtered query returned media under
    CC BY-NC. Used for name resolution only.
11. **A visual hull.** §6.

### OPEN — and the biggest of these is not limb thickness

- **~~Belly clearance is 26% too high across the library and was not fitted.~~**
  **CLOSED 2026-08-15 — see `docs/quadruped-stance-height.md`.** It was not a
  tuning gap: `quad.shoulder_h` is the shoulder JOINT and every value authored
  into 131 specs was a published shoulder (WITHERS) height, so the generator
  drew each animal a half-trunk-depth above the number it was authored from.
  Measured withers ran 1.35× the authored figure with **0 of 108** standing
  species inside 10%. Fitted, it is 1.003× with 106 of 108 inside 10%. The
  girth gate this was feared to fight turned out to move the other way —
  withers is its denominator, so lowering the library carried girth/withers from
  1.00 to 1.25, into the live range, and took the count below the 0.95 retune
  target from 38 of 131 to zero. `belly ÷ length` is **not** what was fitted;
  §4's own denominator turned out to carry most of that 1.26×, and the
  length-free `leg_share` went 1.067× → 0.957×.
- **Trunk width / length**, still 0.25 against Veloren's 0.505. A silhouette is
  the wrong instrument; this needs the **top** view, and PhyloPic has none.
  Commons `Category:Top views of…` was not investigated.
- **Limb taper.** Infinigen authors a proximal and a distal radius per segment
  and a real foreleg is 2.1–2.5× thicker at the forearm than at the cannon. Still
  not in this generator, still the obvious next parameter.
- **Wikimedia Commons as a second corpus** (§2.7), which would cover the species
  PhyloPic has one silhouette or none for.

---

## 8. What could not be sourced, could not be fitted, and is estimated

**Could not be sourced.**

- **Chest girth for any wild species.** Every live limb-girth and heart-girth
  dataset found is a domestic production or companion animal, because the
  measurement exists for breeding and welfare reasons. The horse figure
  (1.07–1.14) is the only verified one in this file.
- **A `Bison bison` limb-length series.** The one dataset that should have had it
  leaves both length cells blank.
- **Shoulder height ÷ head-body length for true *Cervus elaphus*.** ADW's page is
  wapiti; its own title says "elk".
- **Any front, rear or top orthographic reference at all.** PhyloPic is lateral
  only. The brief's five views are, in practice, one view.
- **MorphoSource's current terms of use** — bot protection, no post-2022 archive.
- **Animal3D's 26 keypoint names** — never enumerated in the paper text.

**A fifth GBIF mis-resolution, found on 2026-08-15 by a check that did not exist
before.** `common-frog` had resolved to ***Mustela lutreola*** — a European mink
— because the Mammalia constraint that fixes "brown bear → *Protea speciosa*"
cannot return an amphibian, and two mink silhouettes were sitting on disk under
`refs/silhouettes/common-frog/`. **A verified name is not the same thing as
verified pictures**: `refs/species-latin.json` records what the name was checked
to be and `SOURCES.json` records what the files were downloaded AS, and fixing a
name by hand without re-fetching leaves them disagreeing. `reffit fit` now
refuses any species where the two differ and prints the fetch command. The frog
and `elk-wapiti` (*Cervus elaphus* → *Cervus canadensis*) were re-fetched; the
stale files were dropped by the same name-change path §2.1 already had.

**Coverage, 2026-08-15.** The name gate — not the corpus — was what stopped most
species being fitted. 41 more binomials were hand-entered and put through the
same GBIF check, all 101 verified, taking the species eligible for a limb fit
from **27 to 42** and for a stance fit to **53**.

**Could not be fitted.**

- **104 of 131 species.** 37 reach fit quality but 10 of those have an
  unverified scientific name and are skipped, leaving 27 eligible; 49 more are
  report-only (one silhouette, or the silhouettes disagree beyond the spread
  gate) and the rest have no usable silhouette at all. Reptiles fare worst —
  PhyloPic's lizards are mostly top-down or dorsal, and every one of ours was
  refused by the gate.
- **Six of the fourteen that did move** reached the 1.5× cap rather than their
  reference, and are flagged for a render.
- **Belly clearance and trunk width**, by choice, per §7 OPEN.

**Where this is estimating.**

- **`MAX_SPREAD` and `MIN_SILHOUETTES = 2` are judgement**, tuned against
  what the corpus does, not derived. At n≥4 and spread ≤0.45 only **two** species
  in the whole library qualify, which would have made the exercise empty;
  loosening them was a decision to accept weaker references and defend them with
  a cap instead. It is the same *kind* of call as the 0.16 this file set out to
  replace, and it is recorded as such.

  **REVISED 2026-08-15: the statistic was wrong, not the number.**
  `(max−min)/median` punishes a species for being well sampled — a range can
  only grow with n. Measured over this corpus: median range/median is 0.38 at
  n=2–3, 0.60 at n=4–6 and **0.81 at n=7–11**, correlating +0.67 with sample
  size. That threw out `plains-zebra` (7 silhouettes, and §4 calls it the
  best-sampled species here), `grey-wolf` (11), `reindeer` (9) and `wild-boar`
  (5) — one striding outlier among eleven good drawings is what a median is for,
  and the range gate handed the outlier the verdict. The gate is now the
  **interquartile** spread at **0.275**, which is not a fresh judgement: at n=2
  and n=3 the IQR is exactly half the range, so 0.275 reproduces the old 0.55
  verdict on the sample sizes it was tuned against and only differs from n≥4.
  It admitted 5 species and dropped none.
- **`MAX_LIFT = 1.5`** is a bound, not a measurement.
- **`refsil`'s band 0.30–0.75 of belly height** and the 1.2%-of-length minimum
  run were set by looking at the corpus.
- The **H/L ceiling of 1.70** in `refsil.usable` is a pose filter that bounds
  the wrong quantity (total height, which antlers inflate) and is set loose
  enough to clear a rack. It was 1.45 and at 1.45 it threw away our own
  `red-deer-stag`.
- **`wood-bison` is fitted against *Bison bison* silhouettes** — GBIF accepts no
  subspecies binomial for *B. b. athabascae*, so the reference is the parent
  species. **[estimate]** The same applies to `forest-buffalo` against
  *Syncerus caffer*, though that species did not move.
- **`elk-wapiti` was NOT fitted**, and the reason is the name gate doing its job:
  GBIF lumps wapiti into *Cervus elaphus* and this library keeps them apart, so
  the name was never hand-verified and `fit` skipped it. It is one of **ten
  species with a fit-quality reference and an unverified name** — the others are
  `alpaca`, `arctic-fox`, `common-frog`, `corsac-fox`, `european-wildcat`,
  `fennec-fox`, `golden-jackal`, `striped-skunk` and
  `western-lowland-gorilla` — all printed by `fit` rather than silently dropped.
