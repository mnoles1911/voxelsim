# Markings with a shape: colour boundaries, cephalofoils and sexes

Research behind three additions to `forge/fish.py` on 2026-08-13:
`fish.field_curve`, `fish.head_width` and `fish.sex`. Done the way
`docs/fish-shape-research.md` was done — the literature first, then a
measurement of what survives the lattice, then a list of what was rejected with
the number that killed it.

Companion to `docs/marine-megafauna-research.md`, whose §4 listed all three as
gaps. It also **retracts a number that document asserted** — see §5.

**The finding that shaped the whole exercise.** The delphinid colour-pattern
literature is *topological, not metric*. It records which field borders which,
and which landmark a boundary passes over, and almost never a coordinate. Five
searches turned up exactly three usable published ratios for the geometry of a
cetacean colour boundary, and two of the four numbers this repo already carried
were wrong. Everything else here is either traced from photographs and marked as
such, or rejected.

---

## 1. What was missing, in one sentence

Every marking primitive in this generator was **ink laid on a field** — a band
(`stripe`, `bars`), a blotch (`spots`, `mottle`, `saddle`) or a patch
(`eye_patch`) — drawn over a countershading whose two boundaries were **level
lines** running the length of the animal. The three most recognisable colour
schemes in the sea are none of those things. They are the *boundary itself*
having a shape.

| Scheme | What it is | Species |
|---|---|---|
| **cape** | the dark back reaches down onto the flank at one place and lifts again | every delphinid |
| **flame** | the pale belly throws a blaze up the flank at one place | orca |
| **hourglass** | both at once, meeting, pinching the flank out between them | common dolphin |

---

## 2. Perrin's component grammar, and the rule that decided the design

Delphinid pigmentation has a published component system going back to Mitchell
1970 and Perrin 1972: **cape, dorsal overlay, lateral field, ventral field,
flipper stripe, eye stripe, eye spot, blowhole stripe, mesial rostral stripe,
lip patch, spinal blaze**, plus Mitchell's **thoracic patch** and **flank
patch**.

| Source | Access |
|---|---|
| Perrin 1972, *Color patterns of spinner porpoises*, Fish. Bull. 70(3):983–1003 — [free PDF](https://spo.nmfs.noaa.gov/sites/default/files/pdf-content/1972/703/perrin.pdf) | full text |
| Perrin 1998, *Stenella longirostris*, Mammalian Species 599 — [free PDF](https://academic.oup.com/mspecies/article-pdf/doi/10.2307/3504456/8071497/599-1.pdf) | full text |
| Gwinn & Perrin 1975, Fish. Bull. 73(2):439–443 — [free PDF](https://spo.nmfs.noaa.gov/sites/default/files/pdf-content/fish-bull/gwinn.pdf) | full text |
| Heyning & Perrin 1994, Contrib. Sci. 442 — [OCR](https://archive.org/download/biostor-214999/biostor-214999_djvu.txt) | full text |
| Mitchell 1970, Can. J. Zool. 48(4):717–740 — [doi](https://doi.org/10.1139/z70-129) | paywalled |
| Perrin 2009 / 2018, "Coloration", *Encyclopedia of Marine Mammals* — [doi](https://doi.org/10.1016/b978-0-12-804327-1.00094-7) | paywalled |

**The one paragraph worth more than every number in this document** is Perrin
1972 on *Delphinus*, and it is the generative rule the implementation follows:

> "In *Delphinus* spp. … the dorsal field overlay is less extensive anteriorly
> than the cape, resulting in invasion of the cape by the ventral field and
> yielding a four-part criss-cross pattern with zones of black, buff, gray, and
> white. … the buff 'thoracic patch' … represents the color yielded by the
> pigment of the cape alone, the gray 'flank patch' that of the dorsal field
> overlay alone, and the black dorsalmost area that of the combined effect."

So **four colours are the Boolean overlap of two shapes.** Nobody draws a waist.
That is why `fish.field_curve` bends two boundaries rather than adding an
hourglass-shaped marking: an hourglass is a cape and a flame at the same
station, and it falls out. Gwinn & Perrin 1975 confirmed the mechanism
histologically — diffuse melanin in the thoracic patch, granular in the flank
patch.

**The thoracic patch is drawn in `plume_rufous` and not in a buff, and the
probe is why.** Teaching `--read` that an hourglass counts as a marking (it had
been reporting 0% ink on the one species whose entire identity is a colour
boundary) immediately failed the first colour chosen for it: `plume_buff`
against the silver flank beside it is a WCAG value contrast of **1.05**, and
the floor is 1.5. The right hue was present in the voxels and invisible in the
water — exactly the failure `docs/fish-shape-research.md` §6 put that gate
there to catch. `plume_rufous` is the same warm field two steps darker, at
**2.52**.

### 2.1 The cape's shape, which is the only published curve

Perrin 1998 on the spinner dolphin, and it is a complete, orderable curve:

> "The ventral margin of the cape **dips over the eye, is lowest below the
> dorsal fin, passes dorsally about halfway between the dorsal fin and the
> flukes**, and is parallel to the ventral margin of the lateral field on the
> anterior half of the animal."

Corroborated across taxa: *Delphinus* "cape forms a distinctive **V** below
dorsal fin" ([FAO 1993 key 64a, p.166](https://www.fao.org/3/t0725e/t0725e.pdf));
caped orca ecotypes, cape "widens mid-body as it **dips down adjacent to the
dorsal fin**" then "sweeps upward"
([Pitman & Ensor 2003](https://journal.iwc.int/index.php/jcrm/article/download/813/534)).
The orca and the spinner descriptions are topologically identical, which is what
makes this a general delphinid rule rather than a per-species quirk — and what
makes ONE curve shape with a movable station the right parameterisation.

`fish.curve_at` is therefore authored at the dorsal fin. For the common dolphin
the fin origin is **44–46% of total length** over 90 measured animals
(Heyning & Perrin 1994, Table 2 — snout-to-dorsal-origin, four sex/subspecies
groups, n = 10–37 each).

**No source anywhere gives how far DOWN the cape reaches.** Two independent
search passes. The only absolute dimensions in the entire delphinid
colour-pattern literature are Perrin 2001's eye-stripe width (1–2 cm) and a
"light-gray band (4–6 cm)" beside the cape on a pantropical spotted dolphin.
Perrin's figures carry no scale bars at all; they give each specimen's total
length instead, which is the practical substitute and is how `curve_amount` was
set here.

### 2.2 The orca's ventral flame

Published: the ventral field "has **lobes extending up and back along the tail
stock**" and its boundaries have "a **crisp border**"
([Morin et al. 2024](https://pmc.ncbi.nlm.nih.gov/articles/PMC10966402/), open
access). The second half of that is a direct argument for a hard boundary rather
than a fade, and it agrees with
[Cuthill et al. 2016](https://pmc.ncbi.nlm.nih.gov/articles/PMC5135326/): a sharp
countershading transition is optimal under direct illumination and confers no
advantage at all under diffuse light.

**Nothing else about the panel is published.** Not where the flame starts, not
how high it reaches, not how narrow the white gets between the flippers. The
phrase "lateral flame" returns zero hits for orca in Europe PMC — it is not an
established term. `curve_at 0.78` and `curve_amount 0.34` are traced from
reference photographs and the orca's spec notes say so.

---

## 3. The lattice, which is what kills this feature and not the code

A bent boundary is only a curve if it **moves at least two voxels**; below that
it is a ragged line, which is the pixel-art two-sample floor the rest of this
library is built on. How far two voxels is depends entirely on how deep the
animal is in voxels, and that runs from 3 to 42 across the library.

`python tools/fishprobe.py --marks` prints the minimum usable `curve_amount`
per species. Measured:

| Species | body depth, voxels | `curve_amount` needed for 2 voxels |
|---|---|---|
| shoal-herring | 3 | **0.67** |
| pale-minnow | 5 | 0.40 |
| brown-trout, river-perch | 6 | 0.33 |
| mud-catfish | 7 | 0.29 |
| clown-anemonefish | 8 | 0.24 |
| **bottlenose-dolphin** | **9** | **0.22** |
| reef-shark, river-eel | 10 | 0.20 |
| great-white-shark, tiger-shark | 12 | 0.17 |
| common-dolphin (at 2 cm) | 18 | 0.11 |
| orca | 27 | 0.07 |
| whale-shark | 42 | 0.05 |

**The slider's ceiling is 0.70 of body depth**, so a herring would have to move
its boundary across two thirds of its own flank to shift it two voxels — at
which point the "curve" is the countershading. **A shaped boundary is a
large-animal feature and the small fish are excluded by measurement, not by
taste.** The bottlenose dolphin at 0.22 is the shallowest animal in the library
that can carry one, and it gets exactly two voxels.

### 3.1 The common dolphin chose its own voxel size, and it is the first time a COLOUR feature has

The lattice rule in `docs/marine-megafauna-research.md` §5.2 is "the coarsest
authorable voxel size at which the species' smallest identifying feature is
still about three voxels across". Until now that feature has always been a fin.
For the common dolphin it is the hourglass. Measured:

| Lattice | length | body depth | cape dip | flame rise | waist | voxels |
|---|---|---|---|---|---|---|
| 5 cm | 54 | 7 | **1** | 3 | 3 | 1,797 |
| 2.5 cm | 108 | 14 | 3 | 5 | 6 | 13,676 |
| **2 cm** | **134** | **18** | **4** | **7** | **8** | **26,594** |

5 cm fails outright. 2.5 cm meets the three-voxel rule with nothing to spare,
and `variation.shape` moves body depth by a tenth either way, so it would fail
on some individuals. **2 cm**, which is a tier the library already authors three
species at, and which costs about what a bluefin tuna costs.

---

## 4. The cephalofoil

`docs/marine-megafauna-research.md` §2.3 called this "the strongest single
silhouette in the group and the clearest candidate for the next parameter", and
it was right about why it was hard: the body loft derives width from depth
(`_width_profile` raises the depth profile to a power), and **a hammerhead's
head is the shallowest part of the animal and by a long way the widest**. There
is no exponent that produces that.

### 4.1 Span, from Compagno

Cephalofoil width as a percentage of total length
([FAO Sharks of the World vol. 2](http://www.fao.org/4/ad123e/ad123e35.pdf)):

| Species | width %TL | chord ÷ width | preoral ÷ width |
|---|---|---|---|
| *Eusphyra blochii* (winghead) | **40–50** | not stated | not stated |
| *Sphyrna lewini* (scalloped) | **24–30** | **< 1/2** | 1/5–1/3 |
| *S. mokarran* (great) | 23–27 | < 1/2 | < 1/3 |
| *S. zygaena* (smooth) | 26–29 | < 1/2 | 1/5–<1/3 |
| *S. tudes* | 28–32 | ~2/5 | 1/4–<1/3 |
| *S. media* | 22–33 | 1/2 or more | 1/3–2/5 |
| *S. tiburo* (bonnethead) | 18–25 | **> 1/2** | ~2/5 |
| *S. gilberti* | 25–32 %STL | **0.34** | 0.26 |

Compagno reports the fore-and-aft dimension **already normalised to the width**,
which is exactly the chord-to-span ratio a lofted wing needs. `CEPHALOFOIL_CHORD
= 0.34` is the *S. gilberti* holotype's measured value (nacelle length 47 mm ÷
head width 138 mm) and sits inside Compagno's "less than half" for the three
common species.

**Measurement trap, recorded so nobody repeats it.** Compagno's `HDL = HEAD
LENGTH` runs to the **fifth gill slit** and is NOT the cephalofoil chord — for
*S. tiburo* it exceeds the head width, which would invert the shape. The chord
is his *prebranchial* measure: "snout tip to rear insertions of the posterior
margins of the expanded blades", defined graphically in Figs 17/18/22 and never
in prose.

### 4.2 Thickness, and the defect it caught

Widening the head without flattening it produced a **slab**: the first
hammerhead measured 6 voxels thick across a 27-voxel span, a ratio of 0.22.
The reason is instructive — the wing is widest at 80% of its chord, and by that
station the depth profile has already reached 68% of full body depth, so the
loft was drawing a fat head that happened to be wide.

**No published thickness ratio exists for any sphyrnid.** `HDH = HEAD HEIGHT`
is defined in Compagno's scheme and reported in no species account. The only
number anywhere is the *S. gilberti* holotype's nacelle height, 13 mm against a
head width of 138 — **0.094** — and that single juvenile is what
`CEPHALOFOIL_THICK` is set from.
[Iosilevskii 2020](https://pmc.ncbi.nlm.nih.gov/articles/PMC7657883/) modelled
the cephalofoil as a NACA 0015 section, but that is a CAD assumption and not a
measurement; the same paper's usable result is that the foil stalls past ~12°
angle of attack.

### 4.3 What came out

`python tools/fishprobe.py --head`, on `scalloped-hammerhead` at 5 cm:

| | measured | published target |
|---|---|---|
| head span | 25 vox = **27.5% of total length** | 25–32% |
| chord ÷ span | 8 ÷ 25 = **0.32** | < 0.50; *S. gilberti* 0.34 |
| thickness ÷ span | 2 ÷ 25 = **0.08** | 0.094 |
| eye position | **on the tip**, 25 of 25 | at the tips |

**The eye lands on the wingtip for free**, and that is worth stating because
nothing was written to do it: `_eye` puts the pupil on the outermost occupied
voxel at its station, the station is inside the chord, so widening the head
carries the eye out. The probe measures it precisely because a feature nobody
had to write is a feature nobody would notice breaking.

### 4.4 The review camera had the same bug the flying bird had

A cephalofoil is a **horizontal plate**, exactly like a fluke. The fish review
camera sits at 8°, where a 25-voxel span projects onto 3 voxels of silhouette;
the whale camera sits at 30°, where it projects onto 12. `render.camera_for`
now sends a fish that authors a head span to the high camera — the same
exception `bird.pose` needed, for the same geometric reason.
`out/fish/hammerhead-ab.png` shows both.

---

## 5. ⚠ A retraction: the orca eye patch

`docs/marine-megafauna-research.md` §3, `forge/fish.py::_eye_patch` and
`fish.eye_patch`'s help text all asserted:

> "Measured at **21.8 × 5.9 cm** on a ~6 m animal — 3.6% of body length at an
> aspect of **3.7:1**."

**No such measurement exists.** No study anywhere reports orca eye patches in
absolute units; the elliptical-Fourier work deliberately standardises size out.
The two figures match a pair of dimensionless Simpson-diversity values
(`21.8 ± 4.3` and `5.7 ± 0.8`) in the saddle-patch paper below, which is the
most likely provenance. All three places now say so.

What is actually published about orca pattern geometry:

| Quantity | Value | Source |
|---|---|---|
| Eye patch length ÷ blowhole-to-dorsal-fin distance, Antarctic Type B1 | **0.41** (n=8, SD 0.04) | [Durban et al. 2016](https://doi.org/10.1007/s00300-016-1942-x) |
| same, Type B2 | **0.37** (n=11, SD 0.03; p=0.04) | same |
| Type C eye patch, forward end slants down | ~45° | [Pitman & Ensor 2003](https://journal.iwc.int/index.php/jcrm/article/download/813/534) |
| Saddle width ÷ dorsal fin base width | **0.57**, n=**3,066** | [Mäkeläinen et al. 2024](https://doi.org/10.1111/mms.13171) |
| Saddle shape: smooth | 74.9% of 3,909 | same |
| Eye patch outline | 9 Fourier harmonics = 99.9% of power, n=187 | [Hutchings et al. 2025](https://link.springer.com/content/pdf/10.1007/s42991-025-00523-w.pdf) |

**A second correction**: the saddle-width sample here was recorded as 3,909.
That is the sample for the *shape classification*, a different table; the ratio
is n=3,066 over 47 groups. And Pitman & Ensor's Type B and Type C patches are
the other way round from how they are usually summarised — **B is very large and
parallel to the body axis; C is small and slants down at the front.**

---

## 6. Sexual dimorphism

### 6.1 The design, and why the authored numbers had to move

One choice, `fish.sex` (`unsexed` / `female` / `male`), and three
**male-to-female ratios** — length, dorsal fin height, flipper reach. The male
gets `sqrt(r)` and the female `1/sqrt(r)`, so male ÷ female is exactly `r` and
`unsexed` is the geometric mean.

The alternative — "the authored animal is the female and the male is scaled up"
— needs one number instead of two per trait as well, and was rejected because it
makes every unsexed spec in the library silently female. The square root makes
neither sex the default, at the cost of forcing the question *which animal is
this spec?* to be answered out loud. It turned out three specs could not answer
it:

| Spec | was | is | ratio | why |
|---|---|---|---|---|
| `orca` dorsal height | 1.30 of depth = **27% of body length** | 0.92 = 20% | 1.55 | the published bull is 22–26%; the authored fin was taller than any measured male |
| `whale-shark` length | **9.0 m** | 11.1 m | 0.59 | 9 m is the male asymptote (n=48); the female's is 14.55 (n=6) |
| `sperm-whale` length | **16.0 m** | 13.3 m | 1.45 | 16 m is a bull; cows are 11–12 m (n=1,088 at physical maturity) |

The orca is the instructive one. Its notes said "this is authored male", and its
fin was — but its **flippers were a female's at the same time** (12.6% of body
length against a bull's ~20%). "The orca" in this library was half of each
animal, and nothing on disk said so. Its length did not need correcting: a 1.14
ratio puts 7.5 m and 6.6 m either side of the authored 7.0, which brackets the
published Type A means of 7.3 and 6.4.

Everything else keeps its authored size. **The rule applied was: re-author only
where leaving the number alone would put BOTH sexes outside their published
ranges.** That is three specs, not twenty-three.

### 6.2 Which species carry one, measured

`python tools/fishprobe.py --sex`, individual variation off. Eleven of
twenty-three:

| Species | ratio | source | measured movement |
|---|---|---|---|
| whale-shark | 0.59 L | 8.5 m ♂ (n=48) vs 14.55 ♀ (n=6) | **146 voxels** |
| sperm-whale | 1.45 L | 15.8 m ♂ vs 11.0–12.2 ♀ (n=1,088) | 55 voxels |
| great-white-shark | 0.80 L | mature 350–410 cm ♂ vs 450–500 ♀ | 25 voxels |
| beluga | 1.25 L | 483 cm ♂ (n=130) vs 386 ♀ (n=166) | 23 voxels |
| **orca** | 1.14 L, **1.55 D**, 1.60 P | Type A 7.3/6.4 m; fin 22–26% vs 13–18% | 20 voxels (fin 23→38) |
| reef-shark | 0.87 L | 139 cm ♂ vs 157 ♀ | 14 voxels |
| tiger-shark | 0.88 L | 406 cm ♂ (n=420) vs 464 ♀ | 13 voxels |
| scalloped-hammerhead | 0.89 L | *no Sphyrna figure found*; carcharhiniform cross-species | 11 voxels |
| humpback-whale | 0.94 L | 13.0 m ♂ vs 13.9 ♀ | 9 voxels |
| common-dolphin | 1.05 L | 189.5 cm ♂ (n=28) vs 180.1 ♀ (n=37) | 7 voxels |
| clown-anemonefish | 0.78 L | 52.6 mm ♂ vs 67.5 ♀, n=134 in 67 pairs | 7 voxels |

Sharks are female-biased across the board: **mean female ÷ male = 1.133 over
162 species, 134 of them female-biased**, computed from the deposited dataset of
[Gayford & Sternes 2024](https://pmc.ncbi.nlm.nih.gov/articles/PMC10944705/).
Carcharhinids run 1.10–1.15, lamnids 1.25–1.5, the whale shark ~1.7. The
hammerhead's 0.89 is that cross-species figure and not a *Sphyrna* measurement,
and its spec says so.

The **clownfish** is the one small fish worth a sex and the reason is its
biology: anemonefish live in a size hierarchy where each rank is **1.26 ± 0.10**
times the one below it (n=107 ratios over 70 groups,
[Buston & Cant 2006](http://people.bu.edu/buston/lab/Population_Ecology_files/Buston_Oecologia_2006_1.pdf)),
and the female is simply the largest fish on the anemone. Every one of them
started male. There is **no colour difference** — "sex change does not lead to a
profound modification of the pigmentation pattern in anemonefish"
([PMC7539381](https://pmc.ncbi.nlm.nih.gov/articles/PMC7539381/)) — so bar count
and melanism track species and host anemone and never sex.

### 6.3 Sex reseeds. `bird.pose` does not. Both are right.

The owner ruled on birds that changing an animal's POSE must not change which
individual you get: a raven perched and the same raven flying are one animal, so
`bird.pose` is excluded from the seeding hash (`spec.SEED_INVARIANT`).

**`fish.sex` is deliberately not in that list.** A male orca and a female orca
are two animals, not one animal in two postures; there is no individual that is
"the same whale, but female", so seed 7 male and seed 7 female are two different
whales. `--sex` checks the hashes differ on every species that claims a ratio,
so the decision is verified rather than inherited from wherever the field
happened to be put.

The consequence worth knowing: the A/B render in `out/fish/sex-ab.png` is drawn
with individual variation OFF, which is the opposite of what
`tools/birdprobe.py --pose-ab` does. A pose A/B must leave variation on, because
the defect it hunts is the two poses coming out as two different birds. Here two
different animals is the design, so leaving the draw in would mix the dimorphism
with the dice.

---

## 7. Adopted

**A boundary with a shape, as the fourth marking primitive and the first that is
an EDGE rather than ink.** `fish.field_curve` bends the two countershading
boundaries; `curve_at` says where; `curve_amount` says how far, as a share of
body depth, and 0 turns the whole mechanism off whatever shape is selected.

**The hourglass as two curves rather than as a third shape**, because Perrin
1972 says a *Delphinus* criss-cross is the overlap of two fields and not four
regions. The forward half of the pinched flank takes the marking colour, so a
common dolphin gets its thoracic patch and grey flank patch out of material
roles it already had. **The criss-cross IS that species' one marking**, which is
why `fish.pattern` and an hourglass cannot both be wanted — the same "one
marking, never two" rule the fish research doc set.

**The pale field wins where the two boundaries overlap.** Not obvious, and got
wrong first: the first version clamped the pale edge to stop at the dark one, on
the reasoning that a flank cannot be less than nothing. The orca is exactly the
animal where the two fields already touch — dark 0.62 of its depth, white 0.30 —
so a clamped flame had four hundredths of a body to climb through and moved
zero voxels on the one species it exists for. `fishprobe` reported both curve
parameters DEAD on its first run.

**A hard boundary, not a fade.** Morin et al. 2024's "crisp border" for the
orca, and Cuthill et al. 2016's finding that a sharp countershading transition
is optimal under direct light and useless under diffuse.

**A lateral head widening as an override on the loft's own half-width, applied
as a MAXIMUM.** It can only widen a head, never pinch one; a parameter that
could narrow the snout would be a second and worse `fish.snout`, and on a slim
species it would cut the body in two. It widens the loft rather than adding a
plate, so connectivity is by construction — the same argument the fins lose
every time.

**A head flattening derived from the span**, at the one measured ratio (0.094).

**Sex as one choice plus three male-to-female ratios**, split as a square root
so neither sex is the default.

---

## 8. Rejected, with the number that killed it

**A shaped boundary on the small fish.** Measured (§3): a shoal herring is 3
voxels deep, so moving its boundary two voxels needs `curve_amount` 0.67 — two
thirds of its own flank, at which point the "curve" is the countershading. Six
of the twenty-three species need more than 0.24. This is not a restriction in
the code; the parameter is offered on every swimming species and the probe
prints the floor per species.

**The common dolphin at 5 cm.** Cape dip **1 voxel**. Authored at 2 cm instead,
at 15× the voxels, and that is the first time a colour feature has chosen a
lattice here.

**A boundary-width slider.** The half-width of the bend is a constant
(`CURVE_HALF_WIDTH = 0.22` of body length) and not a parameter, because no
source measures the fore-and-aft extent of a cape dip or a ventral flame, and
because it does not buy anything a reader can see. Swept on the common dolphin
at 2 cm:

| half-width | columns touched (of 134) | cape dip | flame rise | waist |
|---|---|---|---|---|
| 0.08 | 68 | 4 | 6 | 8 |
| 0.14 | 76 | 4 | 6 | 8 |
| **0.22** | **80** | **4** | **7** | **8** |
| 0.32 | 89 | 4 | 7 | 8 |
| 0.45 | 105 | 4 | 7 | 8 |

**A 5.6× change in the parameter moves every voxel measurement by at most one.**
A slider with one defensible setting is a slider that can be set wrong.

**Perrin's mechanism implemented literally**, as two dorsal overlays composited
with a Boolean. It is the correct model and it needs a second downward boundary
whose only user is one species, and it would redefine what `back_frac` and
`belly_frac` mean on all twenty-three. The three-field approximation — cape from
above, ventral field from below, forward flank buff — produces the same four
colours on the animal that has them.

**The cephalofoil's scallops.** The medial and lateral indentations are what
separate *S. lewini* from *S. zygaena* from *S. tiburo* in Compagno's key, and
the only quantitative one is *S. gilberti*'s lateral indentation at **49% of the
half-span**. The built chord is **8 voxels**, so an indentation is one voxel and
reads as a rasterisation error. Same verdict, same reason, as the gill slits.

**A leading-edge sweep angle.** No angle in degrees is published for any
sphyrnid. Compagno is qualitative throughout — *lewini* "very broadly arched",
*mokarran* "nearly straight in adults", *tiburo* "broadly arched or somewhat
angular, without indentations". The quarter-ellipse leading edge here is
"broadly arched" and nothing more is claimed.
[Liu et al. 2026](https://academic.oup.com/iob/article/doi/10.1093/iob/obag024/8324017)
fit `y = ax² + bx + c` to *S. tiburo* margins with |a| from 0.088 to 0.137, but
`a` is scale-dependent and comparative only.

**Sex differences under two voxels.** Measured against the authored size:

| Species | published difference | in voxels | verdict |
|---|---|---|---|
| river-perch | ♀ 6% longer (173 vs 162 mm) | 1.6 | **below the floor** |
| bluefin-tuna | ♂ 3.4% longer (L∞ 286.6 vs 277.3 cm FL) | 4 | the literature says outright it "lacks clear sexual dimorphism" |
| shoal-herring | 0.4% (25.3 vs 25.4 cm) | 0.1 | none |
| reef-tang | none published | 0 | *Paracanthurus* is monomorphic |
| humpback-whale | 0.94 in length **only** | 9 | length adopted; nothing else — the sole other external difference is the female's genital lobe |
| northern-pike | FishBase 40 vs 55 cm | — | **rejected as a source**: FishBase's "male/unsexed" field means "male OR unsexed", not a sex comparison |
| bottlenose-dolphin | males larger in 20 of 29 measures | — | no accessible per-measure fractions; rejected for lack of numbers rather than lack of effect |
| blue-whale | females larger | — | no figure retrieved |

**Female body DEPTH.** Adult female great whites are deeper in the trunk
(girth-to-length slope 0.19 against 0.17, n=163, p<0.01) and female perch are
about 12% deeper for their length. Both need a fourth ratio to serve two
species, and the perch's is 1.6 voxels anyway.

**The sperm whale's head**, which is **40% of body length in males against 30%
in females** — the largest proportional sex difference of any animal here.
`fish.head_frac` positions the eye and the pectoral fins and does not change the
head's bulk; the bulk comes from `depth_at` and `snout`. Scaling `head_frac` by
sex would move the eye and draw the same head, which is a no-op wearing a
parameter's name.

**Colour differences by sex.** None found for any species in the library.
Clownfish explicitly not; whale shark spot patterns are individual fingerprints
with no published sex difference; tiger shark stripes fade with **age**, not
sex. One search summary attributed mating scars to a 2016 tiger shark paper and
that paper's abstract says no such thing — the white shark mating-scar finding
is real and was not generalised.

**Left/right asymmetry**, unchanged from the previous verdict: documented for
orca eye patches (about 48% of animals), orca saddles and whale shark spots, and
at two or three voxels of patch there is no asymmetry to express that would not
read as a mistake.

---

## 9. Not rejected — deferred to the owner

**Claspers.** Mature male sharks carry paired pelvic-fin claspers, and every
published maturity scale uses *"does the clasper project past the pelvic fin"*
as the criterion itself, so it is visible by definition. Measured: **8.8% of
total length** on a mature whale shark (81.0 cm inner on 916 cm TL), 75–106 cm
across eight mature males, and ~11% of TL on a shortfin mako
([PMC4393817](https://pmc.ncbi.nlm.nih.gov/articles/PMC4393817/);
[Natanson 2020](https://spo.nmfs.noaa.gov/sites/default/files/pdf-content/fish-bull/natanson_2.pdf)).
On `great-white-shark` at 108 voxels of total length that is **10 voxels** —
well clear of every floor in this library, so the lattice is not the reason it
is absent.

Two reasons it is not in, and the second is the real one. It sits on the ventral
midline between the pelvic fins, where the 8–30° review cameras see it against
the body for most of its length. And whether an asset library for a game draws
it at all is an owner decision rather than a generator decision, in the same
sense the 5 mm lattice tier was. It is the obvious next male-versus-female
feature for the five sharks, whose only other difference is size.

---

## 10. Sources that could not be read

Recorded so the next person does not spend the search budget again. All
paywalled or bot-walled at the time of writing: Mitchell 1970 full text; Perrin
1997 head stripes; both Perrin "Coloration" encyclopedia chapters; Clark & Odell
1999 (the orca fin study — its ABSTRACT establishes that flipper length and
dorsal height are the only two proportionally dimorphic characters, which is
why `sex_pectoral` exists, but the fractions are not verifiable from it);
Heyning & Dahlheim 1988; Mäkeläinen's 2020 Helsinki thesis
([hdl 10138/318599](http://hdl.handle.net/10138/318599)), which is the most
likely home of unpublished orca ventral-pattern numbers and is worth a human
retrieving.

Nothing was embedded. Every number here is a published figure typed by hand, in
the same way and for the same licence reasons as
`docs/fish-shape-research.md` §4.
