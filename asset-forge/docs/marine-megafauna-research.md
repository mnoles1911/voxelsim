# Sharks, whales and dolphins: what changes when the animal is 25 metres

Research behind the `cetacean` kind and the large-fish species in
`forge/fish.py`. Done before any of them were authored.

Companion to `docs/fish-shape-research.md`, which covers the body loft, the
fins, the colour grammar and the readability floors. This one answers three
questions that document did not have to:

1. **What is actually different about a whale?** (Section 1.)
2. **What is actually different about a shark?** (Section 2.)
3. **What voxel size does a 25 m animal want, and why is it not the one a
   25 cm animal wants?** (Section 5 — the answer is *coarser*, and the reason
   is the opposite of the obvious one.)

---

## 1. Cetaceans are not fish, and it comes down to four parameters

The parts everyone knows — horizontal fluke, flippers, blowhole, no pelvic or
anal fin — are real and each is one parameter. The part nobody mentions is the
cross-section, and that one needed a change to the body model.

### 1.1 The fluke: span is not a constant fraction

| Species | fluke span ÷ body length | source |
|---|---|---|
| Blue whale | **0.215** | Woodward 2006, n=13 |
| False killer whale | 0.219 | Fish 1998 |
| Bottlenose dolphin | 0.236–0.253 | Fish 1998; Pavlov 2021 |
| Beluga | 0.234 | Fish 1998 |
| Harbour porpoise | 0.244 | Pavlov 2021 |
| Gray whale | 0.245 | Woodward 2006, n=223 |
| Orca | 0.251 | Fish 1998 |
| Long-finned pilot whale | 0.270 | Pavlov 2021 |
| Common dolphin | 0.295 | Pavlov 2021 |
| Minke | 0.303 | Pavlov 2021 |
| **Humpback** | **0.341** | Woodward 2006, n=14 |
| **Right whale** | **0.350** | Woodward 2006, n=23 |

Sources:
[Fish 1998, *J. Exp. Biol.*](https://www.wcupa.edu/sciences-mathematics/biology/fFish/documents/1998JEBCompOdontocete.pdf) ·
[Woodward, Winn & Fish 2006, *J. Morphol.*](https://www.wcupa.edu/sciences-mathematics/biology/fFish/documents/2006JMorph.pdf) ·
[Pavlov et al. 2021, *PLOS ONE*, CC-BY](https://journals.plos.org/plosone/article?id=10.1371/journal.pone.0255464)

**A 1.6× spread.** A humpback and a blue whale are not one shape at two sizes,
and a single "cetacean fluke" constant would have been wrong for both. Every
species in `tools/seed_marine.py` carries its own.

Fluke chord ÷ span is about **1/3** for odontocetes (beluga 0.376, orca 0.336,
bottlenose 0.333) and about **1/4** for the large baleen whales. Leading-edge
sweep runs 20–35°, with the orca a measured outlier at 20.7°.

**The fluke is a plate, and one voxel is dimensionally correct.** Thickness
ratio across eight odontocete genera is 0.139–0.232 of chord
([Fish, Beneski & Ketten 2007](https://www.wcupa.edu/sciences-mathematics/biology/fFish/documents/2007AnatRec.pdf)),
so at 80 voxels of body a bottlenose fluke's chord is ~7 voxels and its
thickness ~1–1.6. Minecraft's dolphin fluke is a 10×1×6 slab for the same
reason.

**The median notch is real, universal, and not modelled as geometry.** It is
about 5% of span; span is ~25% of length; so the notch is **1.2% of the
animal** — under one voxel at 80 voxels of body. Every cetacean measurement
protocol on earth defines total length as "snout to fluke notch", so it is not
a minor feature biologically; it is simply below the lattice.
`fish.caudal_fork` cuts one anyway for the species long enough to hold it: at
the authored sizes a blue whale gets 42 voxels of notch and a dolphin 10.

### 1.2 Flippers

| Species | flipper length ÷ body length |
|---|---|
| **Humpback** | **0.308** (n=60) |
| Orca ♂ | ~0.20 |
| Gray whale | 0.174 |
| Right whale | 0.171 |
| Orca ♀ | 0.11–0.13 |
| Blue whale | 0.132 (n=249) |
| Fin whale | 0.08–0.10 |

[Weber et al. 2014, *Mar. Mamm. Sci.*](https://www.wcupa.edu/sciences-mathematics/biology/fFish/documents/2014%20MMS%20Large%20Whlae%20Flipper%20mms12040.pdf)

The famous humpback "one third" is real and is a statistical outlier — longer
than its body length predicts at the 95% CI. Its flipper is also **0.073 of
body length wide**, a chord-to-length ratio of about **0.24**, and that is what
`fish.pectoral_aspect` exists for: before it, a fin's fore-aft extent was a
fixed 10% of body length and could express neither a fish's pectoral nor a
whale's blade. Flippers insert at ~0.25 of body length.

### 1.3 Dorsal fins, and the species that have none

| Species | dorsal height ÷ body length |
|---|---|
| **Orca ♂** | **0.22–0.30** |
| Orca ♀ | 0.13–0.18 |
| White-beaked dolphin | 0.121 |
| Bottlenose dolphin | 0.108 |
| Common dolphin | 0.089 |
| Harbour porpoise | 0.081 |
| **Minke** | **0.040** |
| **Blue whale** | **0.010–0.014** |

Computed from Pavlov 2021 Table 2. **None at all**: beluga, narwhal, right
whales, bowhead. A sperm whale has a hump and a row of knuckles, not a fin.

Falcate versus triangular falls out of the *sign* of the trailing-edge sweep —
positive is the porpoise's triangle, negative is every delphinid's sickle — and
Pavlov's PCA on orca fins found two components, falcateness and base
broadness, reproduce most real variance. Not modelled; `fish.dorsal_shape`'s
four outlines cover the same ground more coarsely.

**A blue whale's dorsal fin is 1.0–1.4% of its length and it is exactly what
separates a blue from a fin from a sei.** This is the single most load-bearing
number in the document and it is why `fish.fin_min_vox` exists.

### 1.4 The cross-section changes along the animal — the one real model change

The literature computes a whale's diameter as **girth ÷ π**, which is to say it
treats the trunk as circular, and that survives peer review across hundreds of
specimens. But the tailstock is explicitly *"highly streamlined and elliptical
in cross-section"*, and modelling it as a cone gives *"anomalously high"*
surface area and volume
([Frontiers in Marine Science, 2019](https://www.frontiersin.org/articles/10.3389/fmars.2019.00334/pdf)).

**A cetacean is a barrel that becomes a vertical blade.** A fish is not — its
section keeps roughly one shape along its length. So the superellipse exponent
had to become two numbers interpolated along the spine (`fish.section` and
`fish.section_tail`) rather than one. This is the only change to the body model
that the whole cetacean exercise needed.

For contrast, two fish measured in section: sailfish 0.28 m tall against 0.12 m
wide (**2.3:1, laterally compressed**); swordfish 0.21 against 0.19
(**1.1:1, essentially round**)
([PLOS ONE 2013](https://journals.plos.org/plosone/article?id=10.1371/journal.pone.0081323)).

### 1.5 Fineness ratio: one body, tuned

Length ÷ maximum diameter, all measured:

| | | | |
|---|---|---|---|
| Blue whale **6.37** | False killer **6.50** | Minke 6.3 | Gray 5.64 |
| Beluga 5.53 | Bottlenose 5.15 | Spinner 5.3 | Pacific white-sided 5.0 |
| **Orca 4.81** | Right 4.58 | **Humpback 4.21** | *(sailfish 9.67, swordfish 6.40)* |

The drag optimum is ~4.5 and the band 3–7 costs under 10% above minimum. Every
cetacean measured sits inside it, and the spread across five species covering a
**6.8× range of body size was under 25%**. That is a strong argument for one
body profile with per-species tuning rather than a hand-authored hull each, and
it is what this generator does.

Maximum girth sits at **0.25–0.40 of body length**; centre of gravity at 0.40.
Tuna are deepest at 0.33–0.40, billfish further forward at 0.24–0.32.

### 1.6 How many stations a whale needs — answered empirically

Hirtle et al. 2022 tested **131,072 combinations** of width-station
measurements against full 3D models of **58 humpback whales** and found volume
error **under 5% from as few as five width stations**
([Dryad, CC0](https://datadryad.org/dataset/doi:10.5061/dryad.kh189328g)).
This generator lofts a continuous profile rather than interpolating stations,
so it is comfortably inside that — but it is the number to reach for if the
profile ever becomes a table.

---

## 2. Sharks: heterocercy is the silhouette

### 2.1 Upper-to-lower lobe ratio

| Taxon | upper lobe | lower lobe | ratio |
|---|---|---|---|
| **Carcharhiniform** (hammerhead holotype) | 31% of TL | 10% of TL | **≈3.1 : 1** |
| **Lamnid** (great white and four others) | ≈equal, lunate | | **≈1.1 : 1** |
| Basking shark | <⅓ of the body | nearly as long | ≈1.1 : 1 |
| Whale shark | strongly heterocercal | grows with age | 2.5:1 juvenile → 1.3:1 adult |
| **Nurse shark** | >25% of TL | absent in young | **≥5 : 1** |
| Thresher | ≈50% of TL | short | ≈4–5 : 1 |

[FAO *Sharks of the World* §1.2](http://www.fao.org/4/x9293e/x9293e01.pdf) ·
[Zootaxa 3702(2):159–178](https://www.biotaxa.org/Zootaxa/article/download/zootaxa.3702.2.5/53233) ·
[Cooper et al. 2020, *Sci. Rep.*](https://www.ebi.ac.uk/europepmc/webservices/rest/PMC7471939/fullTextXML)

**The great white is nearly symmetric**, which is the opposite of how it is
usually drawn, and it is why `fish.caudal_upper` is a slider rather than a
`caudal_shape` entry: the great white and the trout would need the same entry
and different numbers.

**No published per-family angle exists.** Searched hard; Thomson & Simanek 1977
is paywalled and its abstract is qualitative. Compagno's lobe *lengths* are
better anyway — an angle can be derived from them, and cannot be derived from a
quoted angle.

### 2.2 A complete %TL template

Scalloped hammerhead holotype, every value as a percentage of stretch total
length, measured under Compagno's scheme — directly usable as a parameter row:

precaudal 66 · fork 77 · head 24 · pre-first-dorsal 28 · pre-second-dorsal 60 ·
prepectoral 23 · prepelvic 46 · preanal 58 · **cephalofoil width 30** ·
snout 8 · eye 2 · **gill slit heights 3, 3, 4, 4, 3** · pectoral anterior
margin 11 · **first dorsal height 13, base 9** · **second dorsal height 2,
base 3** · anal height 3 · **dorsal caudal margin 31** · **preventral caudal
margin 10**.

Lamnid means over five species, with the paper stating **no allometry within or
between them** — so these are scale-invariant: head **29** · first dorsal
height **10** · caudal span **24** · pectoral length **19**.

### 2.3 What did not get modelled, and why

- **Gill slits.** Each is 3–4% of TL tall and the set spans ~6%. At 80 voxels
  that is one voxel per slit — the Nyquist floor. Below ~80 voxels the honest
  rendering is one dark patch; above ~120, five columns. Not modelled at all,
  because a one-voxel-per-slit rendering reads as a texture error.
- **The hammerhead cephalofoil** (25–32% of TL wide). This needs a lateral
  widening of the HEAD independent of the body's width profile, which the loft
  cannot currently express. It is the strongest single silhouette in the group
  and the clearest candidate for the next parameter.
- **Caudal keels**, **precaudal pits**, the whale shark's **three lateral
  ridges**. All sub-voxel at the authored sizes.

---

## 3. Colour as geometric primitives

Delphinid pigmentation has a published component grammar (Perrin 1972;
Mitchell 1970) and it is already a primitive system: **cape, dorsal overlay,
lateral field, ventral field, flipper stripe, eye stripe, blowhole stripe, lip
patch, spinal blaze**, with four pattern types — striped, crisscross, saddled,
spotted.

**Orca eye patch.** ⚠ **RETRACTED 2026-08-13.** This section used to read
"measured at 21.8 × 5.9 cm on a ~6 m animal — 3.6% of body length at an aspect
of 3.7:1". **No such measurement exists.** A second, independent search found
that no study anywhere reports orca eye patches in absolute units, and the two
figures match a pair of dimensionless Simpson-diversity values in the saddle
paper below. See `docs/marine-marking-research.md` §5. What is published:
patch length is **0.37–0.41 of the blowhole-to-dorsal-fin distance** on the two
large-patched Antarctic types (Durban et al. 2016, n=19); the outline reduces to
**nine Fourier harmonics for 99.9% of its power** over 187 animals (Hutchings
et al. 2025). Shape variation is almost entirely in the anterior end. **About
48% of animals are left/right asymmetric.** Implemented as `fish.eye_patch`,
drawn as a lozenge at 1.85:1 — which is what two voxels of half-height can
hold, and not a claim about the animal.

**Orca saddle patch** is the best-quantified pattern feature found anywhere
(Mäkeläinen et al. 2024, 48 populations): saddle width ÷ dorsal fin base width
has a global mean of **0.57**, **n=3,066** for that ratio (the 3,909 quoted here
before is the sample for the *shape classification*, a different table); shape
is smooth in 74.9% of animals. Implemented approximately as a low-coverage
`saddle` marking.

**Orca ventral panel**: white from the lower jaw, **constricting between the
flippers**, widening, ending past the urogenital region, with a flame that
flares up the flank behind the middle of the animal. That flame is why it reads
as two white shapes from the side. **Modelled from 2026-08-13** as
`fish.field_curve = flame`; the "~0.70–0.75 of body length" this section used to
assert has no source either, and Morin et al. 2024's "lobes extending up and
back along the tail stock" with "a crisp border" is the whole of what is
published. See the marking doc.

**Whale shark**: *"white or yellowish spots and vertical and horizontal stripes
in the form of a checkerboard"*, irregular in front of the pectorals and
regular behind. **No published spot diameter or spacing exists** — two
independent search passes confirmed it. The estimate is 5–10 cm spots on an 8 m
animal (0.006–0.012 of length); the generator's floor put them at 0.02, which
is about four voxels across at the authored lattice. That is a case where the
lattice, not the animal, chose the number, and the spec says so.

**Tiger shark bars**: from ~0.20 to ~0.85 of body length, formed by spots
fusing, and **fading with age**. No source gives a count. Seven is a
construction.

**Countershading sharpness splits by taxon, not by class.** Hard-edged: orca
(*"well-demarked white venter"*), great white (*"abrupt"*), the delphinid
capes. Soft: tuna and most pelagic bony fish, where the transition is silvering
rather than pigment. So cetaceans are on average sharper than fish, but that is
the delphinids and the orca doing it, not being a mammal.

---

## 4. What is not modelled, listed honestly

**Three items left this list on 2026-08-13** and their research is in
`docs/marine-marking-research.md`:

- ~~Hammerhead cephalofoil~~ — `fish.head_width`, measured at 29.7% of total
  length against a published 25–32%. `scalloped-hammerhead` is the species.
- ~~Orca's ventral flame; the delphinid cape curve; the common dolphin
  hourglass~~ — `fish.field_curve`, one primitive covering all three, because
  an hourglass is a cape and a flame meeting rather than a third shape.
- ~~Sexual dimorphism~~ — `fish.sex` plus three male-to-female ratios. Eleven of
  the twenty-three species carry one; the other twelve are measured as having
  none worth drawing and say so.

Still not modelled:

- Gill slits.
- The fluke's median notch as a distinct feature — `caudal_fork` cuts a notch
  but it is the ordinary fork, not the 5%-of-span one a real fluke has.
- The cephalofoil's **scallops** — the medial and lateral indentations that
  separate *S. lewini* from *S. zygaena* from *S. tiburo*. The lateral one sits
  at 49% of the half-span; at nine voxels of chord an indentation is one voxel.
- Ventral pleats (blue whale 55–88, humpback 14–35).
- Left/right asymmetry, which is documented for orca eye patches, orca saddles
  and whale shark spots. At two or three voxels of patch there is no asymmetry
  to express that would not read as a mistake.
- The sperm whale's **head**, which is 40% of body length in males against 30%
  in females — the largest proportional difference between the sexes of any
  animal here. `fish.head_frac` only positions the eye and the pectoral fins;
  it does not change the head's bulk, so scaling it by sex would move the eye
  and draw the same head.
- Sex differences in body DEPTH: adult female great whites are deeper in the
  trunk (girth-to-length slope 0.19 against 0.17, n=163) and female perch are
  about 12% deeper for their length. Both would need a fourth ratio.

---

## 5. The lattice rule: a big animal needs MORE voxels of length

This is the part that is counter-intuitive and it is the reason `resolution_cm`
is per species.

### 5.1 The argument

**A large animal needs more length-voxels not because it is large, but because
its diagnostic features are a smaller fraction of its length.**

| Feature | share of body length |
|---|---|
| Reef-fish dorsal fin | ~25% |
| Humpback flipper | 31% |
| Delphinid dorsal fin | 8–12% |
| Minke dorsal fin | 4.0% |
| Orca eye patch | 3.6% |
| Whale shark spot spacing | ~1.5–3% |
| **Blue whale dorsal fin** | **1.0–1.4%** |
| **Fluke median notch** | **~1.2%** |

A feature needs about **three voxels across it to have a shape** (two to exist
at all — the pixel-art floor, which is really the acuity limit: one resolvable
cycle needs two samples). So the body needs `N ≥ 3 · L / f`:

- delphinid falcate dorsal (10%) → **N ≥ 30**
- orca eye-patch shape (3.6%) → **N ≥ 85**
- **blue whale dorsal fin (1.2%) → N ≥ 250**

The second, independent reason: a constant voxel *count* means the voxel *size*
grows with the animal. A 25 m whale at 32 voxels has **78 cm voxels** — eight
times the terrain's. The "big things need fewer voxels" intuition only holds if
large animals are always far away, and a whale is the thing players swim up to.

Two shipped games agree. Minecraft: tropical fish ~11 px, cod 15, salmon 25,
**dolphin 36**, ender dragon **258** — and its elder guardian is the identical
model **scaled 2.35×**, which is not a different creature, it is a big
guardian. Veloren: clownfish ~18 voxels, marlin ~39, red dragon ~74. Both give
their largest creature **2.5–5× the length-voxels** of their smallest.
([Mojang deobfuscated models](https://github.com/MCFireworkDev/MinecraftDeobfuscated-Mojang/tree/master/minecraft/src/net/minecraft/client/model) ·
[Veloren](https://book.veloren.net/contributors/artists/voxel-models.html))

### 5.2 The rule adopted

> **Choose the coarsest authorable voxel size at which the species' smallest
> identifying feature is still about three voxels across.**

Not "keep every animal at N voxels", because that is what forces 78 cm voxels
on a whale, and not "keep every animal at one voxel size", because that is what
makes a blue whale cost 1.25 billion cells.

### 5.3 What it produced, measured

`python tools/fishprobe.py --lattice`, on the authored specs:

| Species | authored | voxel length | body voxels | one tier finer |
|---|---|---|---|---|
| pale-minnow | 1 cm | 28 | 314 | — |
| brown-trout | 1 cm | 39 | 509 | — |
| reef-shark | 2 cm | 91 | 3,287 | 182 vox, 24,935 |
| bluefin-tuna | 2 cm | 142 | 31,285 | 284 vox, 247,843 |
| bottlenose-dolphin | 5 cm | 64 | 3,194 | 129 vox, 25,202 |
| **common-dolphin** | **2 cm** | **120** | **14,791** | 240 vox, 116,267 |
| **scalloped-hammerhead** | **5 cm** | **80** | **1,445** | 161 vox, 10,748 |
| great-white-shark | 5 cm | 109 | 7,836 | 218 vox, 59,784 |
| orca | 5 cm | 173 | 62,196 | 346 vox, 494,516 |
| whale-shark | 5 cm | 244 | 127,739 | 489 vox, 1,010,510 |
| humpback-whale | 10 cm | 153 | 67,489 | 305 vox, 534,292 |
| sperm-whale | 10 cm | 134 | 30,828 | 268 vox, 245,165 |
| **blue-whale** | **10 cm** | **294** | **232,400** | 589 vox, **1,859,540** |

**The band that came out is 28 to 294 voxels of length**, and it is a
*consequence* of the rule rather than an input to it — the small fish land at
the bottom because their features are large fractions, and the blue whale lands
at the top because its dorsal fin is 1.2%.

Three rows moved on 2026-08-13 and one arrived. `orca`, `whale-shark` and
`sperm-whale` were re-authored onto the average of their two sexes when
`fish.sex` landed (see `docs/marine-marking-research.md` §6), which made the
whale shark 23% longer and the sperm whale 17% shorter; `common-dolphin` is the
first species whose lattice was chosen by a COLOUR feature rather than by a fin.
The rest of the table's small-fish rows moved by a voxel or two as well, because
adding any parameter changes every spec's hash and therefore which individual
seed 1 is. **The longest asset in the library is now a female whale shark at
358 voxels and 479,128 voxels of solid** — twice a blue whale, and reachable
only by asking for that sex.

**What dies outside the band.** Below about 20 voxels, a marking cannot be two
voxels wide and the eye has no contrast partner (see
`docs/fish-shape-research.md` §9). Above about 300, nothing dies — it just
costs: the blue whale is already 232,400 voxels, **three times a temperate
oak**, and one tier finer is 1.86 million and eight times an oak. For an asset
class whose whole premise is that it spawns and despawns cheaply, that is the
real ceiling, and it is a cost ceiling rather than a readability one.

---

## 6. Datasets and licences

**Usable commercially** (all verified downloadable):

- **Dryad is CC0 across the board** ([terms](https://datadryad.org/terms)) —
  [Hirtle et al. 2022, 58 humpbacks with width stations](https://datadryad.org/dataset/doi:10.5061/dryad.kh189328g);
  [Christiansen et al. 2019, 86 southern right whales with girths at three stations and an R script](https://datadryad.org/dataset/doi:10.5061/dryad.m0087p4);
  [McClain et al. 2015 "Sizing ocean giants", 25 species with allometric equations](https://datadryad.org/dataset/doi:10.5061/dryad.411mv).
- **Pavlov et al. 2021**, CC-BY — 56 dorsal fins, 19 flukes, 462
  cross-sections, with span, chord, area, aspect ratio, sweep and per-section
  thickness. Legally clean and exactly a per-station fin table.
- **Gayford et al. 2025**, CC-BY — 54 shark species, 50 from CT scans.
- **ffishAsia on Sketchfab, CC0** — whole-body photogrammetry of real sharks,
  which could be voxelised directly to derive cross-section tables.

**Not usable**: FishBase (CC-BY-NC), SMAL (forbids commercial use including
training), Digital Life 3D's cetacean scans (CC BY-NC), NHM London's blue whale
(NC), Betty et al. 2022's 1,520-animal pilot whale dataset (NC).

**Nothing was embedded.** Every number in `tools/seed_marine.py` is a published
median typed by hand — facts, not a database — and the licences above are
recorded so the question does not have to be reopened.

## 7. Prior art

- **[Infinigen](https://github.com/princeton-vl/infinigen)** (BSD-3) — fish
  bodies as `(9, 8, 3)` station arrays. The detail worth inheriting: its
  species blending pins the temperature to 0.01 for eels and 0.001 for puffers,
  because blending those with anything gives garbage.
- **Cashman & Fitzgibbon 2013, "What Shape Are Dolphins?"** (*IEEE TPAMI*) — a
  morphable dolphin model from silhouettes, four years before SMAL. No public
  model file.
- **[Rune Johansen's procedural creatures](https://blog.runevision.com/2025/01/procedural-creature-progress-2021-2024.html)** —
  503 low-level parameters reduced to 106, and an explicit rejection of PCA
  because the axes are not meaningful to author against. Worth heeding before
  anyone fits a latent space to the tables above.
- **No mature open-source procedural fish or cetacean generator exists.** The
  closest, `jay-salihov/procedural-fish-generator`, has no licence file.
