# Generating bird shapes: what the literature offers, and what survives 25 voxels

Research behind `forge/bird.py`. Done before the generator was written, and the
generator is what it is because of it.

**The question this had to answer** was not "how do you model a bird". It was
"how do you get from a species name to a bird that is twenty to forty voxels
long and still recognisable". Those are very different questions, and almost
everything published answers the first one.

**The finding that shaped everything else**: Minecraft ships universally
recognisable birds at **seven to fifteen voxels along their long axis**, and
humans categorise 32×32 colour photographs at **94% accuracy**. This budget is
two to four times the shipped state of the art. The constraint is not
resolution. The constraint is that at this size you cannot spend voxels on
species-diagnostic detail, because the information does not survive: machine
classification of 200 bird species falls from 51% at 100 px to **31.6% at
25 px**, while *basic-level* recognition — "that is a bird" — is nearly
saturated at 32 px. **Section 9 says what was adopted and what was rejected,
with the numbers.**

---

## 1. GISS: how birds are actually identified

Birders have a word for it. It is spelled *jizz* or *giss*, and it is first
recorded in Thomas Coward's *Manchester Guardian* "Country Diary" of **6
December 1921** — which settles the folk etymology, because that predates the
RAF "General Impression of Size and Shape" backronym by two decades. Greenwood
& Greenwood systematically refuted the RAF, *gestalt* and *gist* theories in
*British Birds*, May 2018
([summary](https://en.wikipedia.org/wiki/Jizz_(birding))).

Coward's own analogy is the useful one here: you recognise a person far ahead
by their carriage and their walk, with no distinguishing feature resolvable.

### Cornell's enumerated procedure

The best authoritative cue list is Cornell Bird Academy's *Inside Birding: Size
& Shape*, at
[academy.allaboutbirds.org/inside-birding-size-shape](https://academy.allaboutbirds.org/inside-birding-size-shape/).
Verbatim:

> "the next thing she'll do is key in on those parts of the bird that are most
> useful for identification: **the head, the bill, the length of the wings and
> the length of the tail**."

Their order is (1) group — "is the bird a duck, is it a heron, is it a
raptor?"; (2) size by comparison — "smaller than a crow but larger than a
sparrow"; (3) **internal proportion** — "comparing an individual bird's body
parts with one another, for example the length of the wings relative to the
length of the tail, can be a particularly good way to identify birds that
appear very similar."

And on why colour is the wrong *primary* cue:

> "While field marks can be helpful, it's not actually what most experienced
> birders use to identify birds… plumages can be variable, especially as they
> change from one season to the next."

Their worked quantitative example is directly usable at twenty voxels — **bill
length measured in head-widths**. A Downy Woodpecker's bill "would barely
extend past the eye" if rotated back over the head; a Hairy Woodpecker's "would
extend well past the eye". Measured: Downy beak 17.4 mm on a 15.5 cm bird,
Hairy 26.6 mm on 22.0 cm.

**This is the entire justification for `bird.bill_frac`, `bird.head_frac`,
`bird.neck_frac`, `bird.body_frac` and `bird.tail_frac` being the first five
rows of the parameter group and for their being NORMALISED to sum to one.** The
cue is proportion. Proportion is a set of shares.

**Honest gap:** no study anywhere quantifies which cue carries the most
identification information for a human. The nearest thing is
layer-wise-relevance work on CUB-200-2011
([IEEE](https://ieeexplore.ieee.org/document/10579007)), which measures what a
CNN uses, not what a birder uses. Treat the ordering above as expert practice,
not as a measurement.

**A second honest gap, and a bigger one:** there is **no published study of how
accurately people identify birds from silhouettes alone**, at species, family
or basic level. A PubMed title search for `silhouette AND bird` returns eight
records, seven of them veterinary radiography of *cardiac* silhouettes and one
about zebrafish. The nearest general result is Wagemans et al. 2008
([DOI](https://doi.org/10.1068/p5825)), which found silhouette identifiability
for everyday objects "ranged from 0% to 100% correct" with no useful mean —
silhouette sufficiency is profoundly object-dependent, which is itself the
finding.

---

## 2. Wing planform: Savile's classification

**Citation:** Savile, D.B.O. (1957), "Adaptive Evolution in the Avian Wing",
*Evolution* 11(2):212–224,
[DOI](https://doi.org/10.1111/j.1558-5646.1957.tb02889.x).

**The original is paywalled and could not be opened.** The class *definitions*
below come from Savile's abstract and from sources that quote him — the
[Wing Ecomorphology lab deck](http://www.pelagicos.net/MARS4040_6040/labs/Mars4040_6040_Fa16_Lab_WingMorphology.pdf),
[Johnson's owl monograph, USDA GTR-NC-190](https://www.nrs.fs.usda.gov/pubs/gtr/gtr_nc190/gtr_nc190_553.pdf),
and [Wikipedia: Bird wing](https://en.wikipedia.org/wiki/Bird_wing). **The
aspect-ratio numbers in §3 are NOT Savile's and are not attributed to him.**
Only one number is directly attributable — Gray Catbird AR 4.7, via Johnson.

| Class | Groups | Tip | Slotting | AR | `bird.wing_shape` |
|---|---|---|---|---|---|
| **Elliptical** | passerines, gamebirds, woodpeckers, forest accipiters | short, rounded | high | low | `elliptical` |
| **High-speed** | swifts, swallows, falcons, plovers, ducks, auks | slender, pointed | none | moderate–high | `pointed` |
| **High-aspect-ratio** | albatrosses, petrels, shearwaters, gulls | long, narrow, flat | none | high | `soaring` |
| **Slotted high-lift** | eagles, hawks, vultures, storks, swans | broad, deeply emarginated primaries | high | moderate | `slotted` |

Savile's own point, which is why this is a parameter and not a taxonomy: each
class "has been developed by several unrelated groups of birds". Wing form is
convergent.

Slotting arises from **emargination**: "abrupt narrowing … in the distal end of
up to five or six of the longest primaries" (Johnson). That is exactly what
`bird.wing_slots` draws, and five or six is why its ceiling is six.

**Sources disagree on ducks.** Wikipedia puts them under high-speed; their
measured aspect ratio (7.3–11.3) is only moderate but their wing loading is the
highest of any group measured (median 144 N/m²). High-speed is defensible;
aspect ratio alone would not tell you so.

---

## 3. Aspect ratio and wing loading, measured

Computed from **Protocol S1 of Alerstam, Rosén, Bäckman, Ericson & Hellgren
(2007), "Flight Speeds among Bird Species", *PLoS Biology* 5(8):e197**
([data](https://doi.org/10.1371/journal.pbio.0050197.sd001)) — mass, wingspan
and wing area for 138 species, built from **33,610 individual measurements**.
AR = b²/S; 129 species had complete biometry.

| Group | n | AR range | **AR median** | wing loading N/m² |
|---|---|---|---|---|
| Divers, cormorant, pelican | 4 | 8.7–12.2 | 10.5 | 132 |
| Gulls & kittiwake | 6 | 9.1–9.9 | **9.7** | 39 |
| Swifts | 3 | 7.4–10.7 | 9.5 | 22 |
| Ducks | 15 | 7.3–11.3 | 9.2 | **144** |
| Pigeons | 2 | 6.7–10.6 | **8.6** | 56 |
| Waders | 16 | 5.7–10.2 | 8.3 | 35 |
| **Falcons** | 6 | 6.9–8.7 | **7.9** | 32 |
| Storks, ibis, crane | 5 | 6.8–8.4 | 7.9 | 75 |
| Swallows & martins | 4 | 7.5–8.8 | **7.8** | **15** |
| Herons | 6 | 5.8–8.5 | 7.2 | 37 |
| **Accipiters** | 2 | 5.8–6.6 | **6.2** | 31 |
| **Eagles** | 5 | 4.2–8.5 | **6.1** | 55 |
| Corvids | 6 | 4.5–6.3 | **6.0** | 35 |
| **Buteos** | 2 | 5.5–5.7 | **5.6** | 30 |
| Small passerines | 22 | 4.4–6.6 | **5.8** | 17 |

**The three raptor shapes, which is why `bird.wing_aspect` is a slider rather
than a consequence of the planform:** falcon 7.9, accipiter 6.2, buteo 5.6. All
three are raptors, all three read differently in the air, and the difference is
one number. Per species: Peregrine 8.28, Kestrel 7.53, Sparrowhawk 5.85, Common
Buzzard 5.72, Golden Eagle 6.90, Raven 5.92, **Eurasian Jay 4.53** — the lowest
in the dataset, and the reason the jay's wing in this library is so short and
round.

Independent checks: Norberg (1990, p. 239) reviewing 141 species gives an AR
range of **4.4 to 17.2**; the computed range over Alerstam's 129 is 4.2–12.2,
consistent at the bottom and lower at the top only because Alerstam has no
albatrosses. Albatross AR is quoted at 15 and 18 by Johnson and by the pelagicos
deck.

**One trap worth stating.** Alerstam follows Pennycuick's protocol, in which
wing area *includes the body strip between the wings*. Johnson measured that
strip separately on 113 owls and found it averages **15.4%** of the total. So a
wing-only chord is 0.85 of what span ÷ AR says, and `forge/bird.py` applies
exactly that factor. Never mix AR numbers across the two conventions.

---

## 4. Hand-wing index, and the shape of a "soaring" wing

AVONET publishes **HWI = 100 × Kipp's distance ÷ wing length** for all 11,009
species: the fraction of the wing that is "hand". High is pointed and
elongated, low is broad and rounded. It is measurable on a folded museum skin,
which is why a dataset this size exists.

Medians computed from AVONET (n = 11,009): min 0.1, p25 15.2, **median 21.0**,
p75 31.5, p95 60.8, max 74.3.

| Family | HWI | | Family | HWI |
|---|---|---|---|---|
| Apodidae (swifts) | **68.6** | | Turdidae (thrushes) | 23.9 |
| Diomedeidae (albatrosses) | 64.6 | | Alcedinidae (kingfishers) | 22.4 |
| Trochilidae (hummingbirds) | 63.7 | | Picidae (woodpeckers) | 21.4 |
| Laridae (gulls, terns) | 60.1 | | Corvidae | **19.4** |
| Hirundinidae (swallows) | 51.4 | | Paridae (tits) | 17.7 |
| Falconidae | 46.9 | | **Troglodytidae (wrens)** | **11.6** |
| Anatidae (ducks) | 46.1 | | Spheniscidae (penguins) | 5.1 |
| Accipitridae | 34.1 | | Struthionidae | 0.1 |
| Ardeidae (herons) | 32.1 | | | |
| Columbidae (pigeons) | 31.3 | | | |
| Cathartidae (vultures) | 27.5 | | | |
| Strigidae (owls) | 27.2 | | | |

By habitat, medians: dense (forest interior) **18.7**, semi-open 20.6, open
**34.9**. By migration: sedentary 19.3, partial 31.2, long-distance 33.2.
**Habitat openness separates wings better than migration does.**

**The single most useful result in this section, and it changed a species
spec.** "Soaring" is not one wing:

| | HWI |
|---|---|
| Dynamic soaring, ocean — albatrosses, petrels | **60–67** |
| Thermal soaring, land — eagles, buzzards, vultures | **27–39** |

Vultures and eagles get their low wing loading from **area**, not from
pointedness. Rendering a golden eagle with an albatross silhouette is a visible
error, and it is the reason `soaring` and `slotted` are two entries in
`bird.wing_shape` rather than one.

---

## 5. Proportion: the numbers the generator is authored from

### Wingspan ÷ length and tail ÷ length, per group

Computed by joining 94 Cornell species accounts (total length, wingspan) to
AVONET tail lengths, 88 species joined.

| Group | n | **span ÷ length** | **tail ÷ length** | HWI |
|---|---|---|---|---|
| Hummingbird | 2 | 1.19 | 0.30 | 65 |
| Gamebird | 3 | **1.27** | 0.34 (to **0.75**) | 26 |
| Small passerine | 7 | 1.45 | **0.40** | 20 |
| Waterfowl | 8 | 1.45 | **0.17** | 49 |
| Heron | 5 | 1.57 | **0.14** | 32 |
| Woodpecker | 3 | 1.58 | 0.36 | 27 |
| Columbid | 3 | 1.58 | 0.43 | 37 |
| Corvid | 4 | 1.64 | 0.41 | 30 |
| Kingfisher | 1 | 1.68 | 0.30 | 34 |
| Accipiter | 3 | 1.88 | 0.46 | 32 |
| Wader | 8 | 1.89 | 0.26 | 51 |
| Swallow | 3 | **2.05** | 0.40 | 53 |
| Gull | 2 | 2.16 | 0.26 | 54 |
| Falcon | 5 | **2.24** | 0.43 | 49 |
| Buteo | 4 | **2.38** | 0.44 | 36 |
| Owl | 9 | 2.44 | 0.37 | 30 |
| Eagle | 2 | **2.54** | 0.38 | 39 |
| Albatross | 2 | 2.73 | **0.19** | 65 |
| Osprey | 1 | **2.95** | 0.36 | 45 |

**The whole design space is 1.11 to 3.12 — under a threefold spread.** That is
the range `bird.wing_span` covers, and it is much narrower than a fish's depth
ratio (a factor of eight). Wingspan is not where bird identity lives. Tail
share is: 0.10 to 0.75, a factor of seven.

**A measurement artifact that matters and that would have produced wrong
birds.** "Total length" is bill-tip to tail-tip, so **a long tail inflates the
denominator and crushes span ÷ length**. Black-billed Magpie measures 1.11 —
the lowest in the dataset — purely because its tail is 50% of its total length;
on body length alone it is about 2.2. Ring-necked Pheasant measures 1.18 with a
tail 75% of its length. `bird.wing_span` is authored against total length
because that is what field guides quote, so the same deflation is baked into
the species values consistently; anyone reading a wingspan out of a table and
typing it in should know the ratio moves with tail share.

### Data quality, stated rather than assumed

Cornell wingspans were cross-checked against an independent
[empirical wingspan database](https://figshare.com/articles/dataset/Avian_wingspan_database_empirical_and_predicted_wingspan_for_10_618_flighted_bird_species/32089083):
median disagreement 4.9%, 50 of 62 within 10%. Specific rows are bad, and one
is badly wrong: **Eurasian Collared-Dove, Cornell 35 cm against the database's
49.9 cm, −29.8%.** Barn Owl differs by 15%, Peregrine by 11%. **Both wrens are
unsupported** — neither has an empirical wingspan, and the two Cornell figures
imply span ÷ length of 2.23 and 1.25 for birds whose wing chords are within
15% of each other, so at least one is wrong. That is awkward, because a wren is
the natural low-resolution test case.

### Family ratios from AVONET

Normalised to wing chord, family medians over 11,009 species:

| Family | tail/wing | tarsus/wing | beak/wing |
|---|---|---|---|
| Apodidae (swifts) | 0.40 | **0.08** | 0.06 |
| Hirundinidae | 0.52 | 0.10 | 0.10 |
| Troglodytidae (wrens) | 0.85 | 0.36 | 0.31 |
| Corvidae | 0.90 | 0.24 | 0.21 |
| Alcedinidae (kingfishers) | 0.64 | 0.15 | **0.50** |
| **Ardeidae (herons)** | 0.36 | **0.33** | 0.32 |
| **Ciconiidae (storks)** | 0.41 | **0.41** | 0.44 |
| **Gruidae (cranes)** | 0.37 | **0.45** | 0.25 |
| Anatidae (ducks) | **0.34** | 0.17 | 0.20 |
| Phasianidae | 0.54 (to **3.07**) | 0.28 | 0.15 |

Three things fall straight out and all three are in the generator:

- **HWI separates the aerial specialists from the dense-cover birds** in one
  number — swift 69 against wren 11.6. That is Savile's axis as a scalar.
- **Leg length is the wader signature**: tarsus ÷ wing 0.33 / 0.41 / 0.45 for
  herons, storks and cranes, against 0.08–0.10 for swifts, swallows and
  hummingbirds. A five-fold spread that no other cue matches. `bird.leg_len`.
- **Bill is the kingfisher signature**: beak ÷ wing 0.50, the highest of any
  family, on a very short tarsus. A kingfisher is a bill and a head with a bird
  behind it.

---

## 6. Bills

### Depth ÷ length by diet, computed from AVONET (medians, n = 11,009)

| Trophic niche | n | **depth : length** | width : depth |
|---|---|---|---|
| Granivore | 694 | **0.537** | 0.79 |
| Vertivore (raptor) | 319 | 0.501 | 0.69 |
| Scavenger | 22 | 0.414 | 0.71 |
| Terrestrial herbivore | 95 | 0.388 | 0.95 |
| Frugivore | 1,202 | 0.330 | 0.86 |
| Omnivore | 1,921 | 0.318 | 0.80 |
| **Aquatic herbivore (ducks)** | 83 | 0.299 | **1.17** |
| Invertivore | 5,311 | 0.262 | 0.93 |
| Aquatic predator (heron) | 797 | 0.214 | 0.73 |
| Nectarivore | 555 | **0.100** | 1.00 |

**Depth ÷ length spans a clean 5.4× from nectarivore to granivore and orders
exactly along function: needle → spear → probe → generalist → crusher.** That
is `bird.bill_depth`, and the numbers above are what the species specs are set
from.

**Width ÷ depth is a second, independent axis and it is the one people forget.**
Below 0.8 is a deep narrow blade (raptors, gulls); about 1.0 is square
(woodpeckers); above 1.2 is a flattened wide gape (ducks, nightjars,
flycatchers). Aquatic herbivores are the only group wider than deep. That is
`bird.bill_gape`, and it is why a mallard and a heron with the same bill length
are not confusable.

Extremes, all computed: stubbiest is *Eupsittula nana*, a parakeet, at
**1.267** — genuinely deeper than long, and the top ten are all parrots and
cockatoos. Most needle-like is *Ensifera ensifera*, a sword-billed
hummingbird, at **0.031** — a 97.4 mm culmen on a 3.0 mm depth.

### Morphospace

Cooney et al. 2017, *Nature* 542:344–347,
[PMC5321581](https://pmc.ncbi.nlm.nih.gov/articles/PMC5321581/): 3D scans of
**2,028 species**, four landmarks plus three semilandmark curves at 25 points
each. **PC1 = 58% of variance and it is the elongated-versus-stout axis** —
exactly the depth ÷ length ratio above, with *Ensifera* and *Geospiza
magnirostris* at its two ends. PC2 = 29%. Eight PCs cover >99%.

**A result that constrains how much bill shape is worth.** Navalón et al. 2019,
*Evolution*, [DOI](https://doi.org/10.1111/evo.13655), found **diet accounts for
under 12% of beak-shape variation** across modern birds. Dinnage & Kleineberg
2025, [PMC11940575](https://pmc.ncbi.nlm.nih.gov/articles/PMC11940575/), got
**60% accuracy** predicting trophic niche from 3D bill shape alone, while Pigot
et al. 2020, [DOI](https://doi.org/10.1038/s41559-019-1070-4), got **70–85%**
using all nine morphological traits. **Bill alone is a weak cue; bill plus wing
plus tail plus leg plus size is a strong one.** That is the argument for a bird
generator having thirty parameters rather than five.

**Not found, and searched for:** bill length as a ratio of *head* length is a
birding heuristic and not a published metric. Bill *curvature* at comparative
scale does not exist either — the standard index is arc ÷ chord (Sustaita et
al. 2025, [PMC12669840](https://pmc.ncbi.nlm.nih.gov/articles/PMC12669840/)) but
no large dataset publishes it, and AVONET does not record curvature.
`bird.bill_curve` is authored by eye against named species.

---

## 7. Tails

The formal definitions, from
[Lederer](http://avesbiology.com/birdtails.html), reduce to **one signed scalar
plus an abruptness parameter**. Let *d* = outer rectrix minus central rectrix:

- *d* < 0 → rounded (slight) → graduated (abrupt) → pointed (extreme)
- *d* = 0 → square
- *d* > 0 → emarginate/notched (slight) → forked (abrupt) → streamered

**`bird._tail_length` is that scalar written as a function**, and it is why all
seven shapes are four lines of code. It is also the direct reason
`bird.tail_fork` applies only to `forked` and `notched`: those two ARE the
positive half of the axis and the fork depth is the abruptness.

**A negative result worth knowing:** there is **no published numeric boundary**
between emarginate, notched and forked. Lederer's distinction is "slight" versus
"abrupt" and nothing more. Europe PMC does not index the journals where tail
graduation is quantified.

### Fork depth, measured

Barn Swallow, Nam et al. 2018, *Zoological Studies* 57:43,
[PMC6517734](https://pmc.ncbi.nlm.nih.gov/articles/PMC6517734/), 82 males and
92 females. Fork depth = T6 − T1:

| | male | female |
|---|---|---|
| Outermost rectrix T6 | 88.1 ± 7.9 mm | 76.2 ± 4.2 |
| Streamer (T6 − T5) | 31.3 ± 6.5 | 20.4 ± 3.0 |
| **Fork depth (T6 − T1)** | **45.8 ± 7.7** | 32.7 ± 4.1 |
| **Fork ÷ tail length** | **52.0%** | 42.9% |

**52% is the number `barn-swallow`'s `tail_fork` is set to.**

Genus-wide, all 14 *Hirundo*, from Hasegawa, Arai & Kutsukake 2016,
[PMC4739571](https://pmc.ncbi.nlm.nih.gov/articles/PMC4739571/) Table S1 (data
from Turner & Rose 1994): fork ÷ wing runs from **0.81** (Blue Swallow) to
**0.05** (*H. nigrita*) — an eighteen-fold range inside one genus. All three
species with fork depth over 50 mm are migratory, and fork depth *decreases*
with bill size.

### Why forked and graduated are different animals

Thomas 1993, *Phil Trans R Soc B* 340:361–380,
[DOI](https://doi.org/10.1098/rstb.1993.0079): tail lift is proportional to the
**square of the maximum continuous span**. So a forked tail keeps the outer
tips — full span, full lift — while removing area and mass from the middle, and
is nearly free; a graduated tail adds length *behind* the span-generating edge,
which costs mass and drag and contributes little. Confirmed empirically by
Balmford, Jones & Thomas 1994, *Evolution* 48:1062–1070: across 57 long-tailed
species, wing-length dimorphism is greater in species with "aerodynamically
costly graduated tails". **Birds with graduated tails have to grow bigger wings
to pay for them.**

Rectrix counts, from Kiat & O'Connor 2024, *PNAS* 121(8),
[PMC10895369](https://pmc.ncbi.nlm.nih.gov/articles/PMC10895369/), 346 taxa:
volant non-hydrodynamic birds carry 3–13 **pairs**, mean 6.09, and **12
rectrices is the modal count**. Model fit of rectrix number against locomotion:
**r² = 0.15**. Count is not where recognisability lives; shape is. Nothing here
draws individual feathers.

**AVONET records tail LENGTH and nothing else** — no shape, no fork depth, no
graduation, no count. No dataset found records tail shape at scale.

---

## 8. Plumage

### What is actually common

Somveille, Marshall & Gluckman 2016, *PeerJ*,
[PMC5111890](https://pmc.ncbi.nlm.nih.gov/articles/PMC5111890/), scored **8,006
species** (80% of all birds):

- **Irregular patterning 24%. Regular patterning 23%. So roughly half of all
  birds are essentially unpatterned.**
- Within regular: **barred 66%**, spotted 30%, scaled 25%.
- **And the key negative result: no association between habitat openness and
  pattern type.** McFadden R² 0.001–0.039, median **0.0065**, over 2,756
  species. The intuitive "barred means forest, streaked means open" story is
  not supported by the only global test of it.

That last one is why `bird.body_mark` is a free choice with no habitat coupling
and no automatic derivation from the biome weights. It would have been an easy
and completely unfounded feature.

### Colour, and the fact that decided the palette

Delhey 2015, *Sci Rep*,
[PMC4683462](https://pmc.ncbi.nlm.nih.gov/articles/PMC4683462/), CC BY 4.0:
**46,559 reflectance spectra, 17 standardised plumage patches, 2,734
specimens, 555 species** (99% of Australian landbirds).

| Mechanism | % of patches | % of colour volume |
|---|---|---|
| **Melanin** | **74.2%** | **6.8%** |
| Carotenoid | 11.6% | 21.1% |
| **Structural** | **7.3%** | **44.8%** |
| Psittacofulvin | 2.6% | 17.7% |

**That inversion is the single most useful colour fact found in this whole
exercise.** Melanin does three-quarters of the plumage area and occupies 7% of
the gamut; structural colour does 7% of the area and occupies 45% of the gamut.
**A palette weighted by area — which is what copying a field guide gives you —
is browns and greys.** To make a bird library colourful you have to deliberately
over-weight the rare colours, and that is a stylisation with a number attached
rather than a matter of taste. It is what `docs/bird-colour-proposal.md` asks
for and what the species notes record.

Corroborated by Delhey et al. 2023, *PNAS*,
[PMC10450850](https://pmc.ncbi.nlm.nih.gov/articles/PMC10450850/), over 9,186
species: "the most common colors are black, white, gray and brown, while the
rarest are green, blue, purple, and red", and ornamental colours generally
cover under 1% of image area.

### Which marks are diagnostic

**CUB-200-2011**, the standard expert-annotated bird dataset
([Caltech](https://www.vision.caltech.edu/datasets/cub_200_2011/)), carries 312
binary attributes. Its pattern enums are the clearest available statement of
expert priority:

- `has_breast_pattern`, `has_back_pattern`, `has_tail_pattern`,
  `has_belly_pattern`, `has_wing_pattern` — **four values each**: solid,
  spotted, striped, multi-coloured.
- `has_head_pattern` — **eleven values**: spotted, malar, crested, masked,
  unique, eyebrow, eyering, plain, eyeline, striped, capped.

**The head is the only region granted a rich vocabulary — nearly three times
the terms of anywhere else.** That is the direct justification for
`bird.head_mark` having six entries where the wing and body marks have five,
and for the head having its own marking material.

Independently corroborated: López-Idiáquez et al. 2025, *J Anim Ecol*,
[PMC12214448](https://pmc.ncbi.nlm.nih.gov/articles/PMC12214448/), 58 of 63
Paridae over 22 plumage patches — species recognition is "more strongly
associated with colouration complexity of the **head and breast** than with
that of the **back and wing**".

### The developmental structure, which constrains what is even possible

Three levels, and the literature is unusually clean.

**Within one feather.** Prum & Williamson 2002, *Proc R Soc B* 269:781–792,
[PMC1690965](https://pmc.ncbi.nlm.nih.gov/articles/PMC1690965/): pattern is set
by differential pigmentation of keratinocytes within **independent barb
ridges**. Their reaction–diffusion model reproduces the complete achievable
set — "a central patch, a hollow central patch, concentric central patches,
**bars**, chevrons, a central circular spot, rows of paired spots, and arrays of
offset dots". Lin et al. 2013, *Science* 340:1442,
[PMC4144997](https://pmc.ncbi.nlm.nih.gov/articles/PMC4144997/), gives the
cellular basis: **the proximal–distal axis of a feather is a time axis**, so
cycling the melanocyte progenitors on and off writes bars.

**Across a feather tract.** Haupaix et al. 2018, *Science* 361,
[DOI](https://doi.org/10.1126/science.aar4777): "stripes form in a two-step
process. Autonomous signaling from the **somite** sets stripe position by
forming a composite prepattern marked by the expression profile of **agouti**."
Instruction first, self-organisation second. Inaba & Chuong 2020,
[PMC7365947](https://pmc.ncbi.nlm.nih.gov/articles/PMC7365947/), names the two
levels: **macro** = "color patterns spanning whole-body regions or specific skin
domains like head, back, wing, and tail — most birds exhibit a specific color on
a specific region"; **micro** = within a vane.

**Three consequences that are directly in the code:**

1. **Bars on a feather are necessarily transverse**, because the mechanism is
   temporal. There is no such thing as a longitudinal stripe within one
   feather. Longitudinal streaking on a bird is a *tract-level* mosaic of
   dark-shafted feathers — which is why `streaked` is drawn as a periodic
   function across the body and `barred` as one along it, and not the other way
   round.
2. **A wing bar is a tract-level phenomenon built from feather-level pale
   tips**: one row of coverts, each with a terminal patch, aligned by
   overlapping tract geometry. That is exactly why wing bars are crisp,
   straight and survive downsampling — and it is why `bird.wing_mark` draws a
   bar at a fixed chordwise position running the whole way out, rather than a
   stripe down the middle of the wing.
3. **Head marks follow anatomical landmarks, not feather boundaries**, because
   they are somite/agouti prepattern. Hence bilaterally symmetric and
   hard-edged, which is how `_head_mark` draws them.

### Marks with real evidence behind them

| Mark | Evidence |
|---|---|
| **White outer tail feathers** | The best-evidenced mark in the literature. Mumme 2023, *Proc R Soc B*, [PMC10685110](https://pmc.ncbi.nlm.nih.gov/articles/PMC10685110/) — 625 hooded warblers over 14 years, **stabilizing selection with peak survival at a tail-white index of 0.50–0.53**, quadratic p = 0.0038. |
| **Black wingtips** | Goumas 2022, *Commun Biol*, [PMC9681726](https://pmc.ncbi.nlm.nih.gov/articles/PMC9681726/) — **50 gull species**: per standard deviation of wing loading, the ratio of black to non-black at the wingtip rises by a factor of **1.4**. |
| **Contrasting wing panels** | Zheng et al. 2022, *Proc R Soc B*, [PMC9257291](https://pmc.ncbi.nlm.nih.gov/articles/PMC9257291/) — 1,780 species, 30 orders: mass × colonial breeding coefficient 0.973 (p = 0.003). |
| **Flash marks / rump patches** | Loeffler-Henry et al. 2021, [PMC8316788](https://pmc.ncbi.nlm.nih.gov/articles/PMC8316788/) — "flashers with long flight-initiation distance survived at about **twice the rate** as non-flashers". |
| **Barred plumage** | Gluckman & Cardoso 2010, [DOI](https://doi.org/10.1111/j.1420-9101.2010.02109.x) — dual function; Marques et al. 2016, [DOI](https://doi.org/10.1098/rsos.160195) — the **regularity** of the barring is the signal. |
| **Wing bars** | Janas et al. 2021, [PMC8599086](https://pmc.ncbi.nlm.nih.gov/articles/PMC8599086/) — a blue tit's white wing stripe "lacked melanosomes": it is **structural**, not white pigment. |
| **Bibs / status badges** | **Be sceptical.** Sánchez-Tójar et al. 2018, *eLife*, [PMC6234027](https://pmc.ncbi.nlm.nih.gov/articles/PMC6234027/) — meta-analysis of the house-sparrow badge: "the overall effect size … was small and uncertain", with demonstrated publication bias. |
| **Iridescent gorget** | Simpson & McGraw 2018, [DOI](https://doi.org/10.1111/ele.13125) — "male position relative to the sun was the strongest predictor of colour appearance". **Angle-gated, not a colour.** |
| Eye-ring, supercilium, tail bands, duck speculum | **No functional literature found for any of them.** They exist as descriptive categories only. |

### Countershading

**The number you would want does not exist.** No study reports what percentage
of bird species are countershaded, or measures dorsal-versus-ventral
reflectance across birds. Five searches. Delhey et al. 2023 has the data —
9,186 species, per-pixel CIELAB — and explicitly "considered only the
proportion of the body … ignoring the spatial distribution", noting in passing
that species "are often darker above than below" without measuring it.

What *is* measured is more useful anyway. **Cuthill et al. 2016, *PNAS*
113:13093, [PMC5135326](https://pmc.ncbi.nlm.nih.gov/articles/PMC5135326/)**:

> "optimal countershading is **strongly illumination-dependent**. A relatively
> **sharp** transition in surface patterning from dark to light is only optimal
> under **direct solar illumination**; if there is diffuse illumination from
> cloudy skies or shade, the pattern provides no advantage."

And Penacchio et al. 2017, [PMC5651847](https://pmc.ncbi.nlm.nih.gov/articles/PMC5651847/):
countershading breaks down beyond **15 degrees** of body orientation relative to
the light. Penacchio et al. 2018,
[PMC5830711](https://pmc.ncbi.nlm.nih.gov/articles/PMC5830711/): wrong
countershading performs like uniform colour — it is worth nothing, not
something.

**`bird.upperparts` is therefore ONE hard boundary and not a gradient**, which
is the "direct solar illumination" case, and which is also what every field
guide draws.

For seabirds specifically, Götmark 1987, *Anim Behav* 35:1786, found
black-headed gulls caught about **34% more fish** with white underparts than
when dyed black — *(that figure is snippet-sourced and was not read from the
paper; verify before quoting it anywhere it matters)*.

---

## 9. Readability at very low resolution

### The budget, from three directions

**Minecraft's shipped geometry**, re-measured from
[github.com/Mojang/bedrock-samples](https://github.com/Mojang/bedrock-samples)
(16 units = 1 block):

| Entity | Body | Head | Bill | Wing | Wing thickness | Bounding box |
|---|---|---|---|---|---|---|
| **parrot** | 3×6×3 | 2×3×2 | 1×2×1 + 1×1.7×1 | 1×5×3 | **1 (solid box)** | 4 × 14.6 × 7 |
| **chicken** | 6×8×6 | 4×6×3 | 4×2×2 | 1×4×6 | **1 (solid box)** | 8 × 15 × 11 |
| bat | 3×5×2 | 4×3×2 | — | 2×7×0 | **0 (plane)** | 19 × 13 × 2 |
| phantom | 5×3×9 | 7×3×5 | — | 6×2×9 | 2 then 1 | 43 × 4 × 25 |
| allay | 3×4×2 | 5×5×5 | — | 0×5×8 | **0 (plane)** | 5 × 11 × 11.5 |
| bee | 7×7×10 | — | — | 9×0×6 | **0 (plane)** | 21 × 9 × 15 |

**The wing rule falls straight out: entities whose wings FOLD against the body
use one-unit solid boxes; entities that are only ever seen with wings spread
use zero-thickness planes.** Only the two largest fliers deviate, and the ender
dragon uses the construction you would want if a wing must read at both close
and far range — a solid spar box plus a zero-thickness membrane.

Nose to tail, Minecraft's birds are **11.0 to 14.6 units** — seven to fifteen
voxels. **This library's 20–40 is two to four times the shipped state of the
art.**

**Human recognition.** Torralba 2009, *Visual Neuroscience* 26:123,
[PDF](https://people.csail.mit.edu/torralba/publications/howmanypixels.pdf):
scene categorisation over 12 categories reaches **81.8% at 32×32 in colour**
and 73% in greyscale, against a 96% ceiling at 128×128. From *80 million tiny
images*: "for grayscale images, humans need around 64 × 64 pixels. When the
images are in color, humans need only 32 × 32… **32 × 32 color images are the
minimum viable size for recognition.**"

**Colour is worth roughly a 2× resolution multiplier.** That is a measured
justification for spending eleven material slots on plumage rather than four.

**Species recognition is a different regime and is not achievable here.**
Cai et al. 2017, [arXiv:1703.05393](https://arxiv.org/abs/1703.05393), on
CUB-200's 200 species: AlexNet scores **51.0% at 100 px, 45.0% at 50 px and
31.6% at 25 px** — and note the acceleration, 6 points lost for the first
halving and 13.4 for the second. CIFAR-10 has a literal `bird` class at 32×32
and humans score **93.9%** on it overall
([arXiv:1811.07270](https://arxiv.org/abs/1811.07270)) — but that is
basic-level, not species. *(The per-class bird figure appears only in an
unlabelled figure in that paper; do not let anyone quote a number for it.)*

**So: aim at "that is a raven and that is a heron", not at "that is a hooded
crow rather than a carrion crow".** The library's twenty species are chosen to
be distinguishable from each other, which is a much weaker requirement than
being identifiable in the field, and it is the right one.

### Craft rules, from the people who do this

[Blockbench's Minecraft style guide](https://www.blockbench.net/wiki/guides/minecraft-style-guide/),
verbatim: **"Small objects being recognizable takes priority over being to
scale (e.g. a bee would have to be smaller than 1px if it were perfectly
proportionate)."** And: "Depicting slants and curves as stairs needs to be
avoided." In axis-aligned cubic voxels the second reduces to: do not build
shape out of many small steps.

Zach Soares, [*Voxel Art: Reducing the Greebles*](https://sketchfab.com/blogs/community/voxel-art-reducing-greebles/),
names the same failure and replaces it with **colour** rather than geometry —
"1 color as the base value for a property and a second color… as the highlight
which would contour the objects" — and notes "when you apply shadows in-game,
everything gets blurred out."

**The single most useful sentence found in this whole exercise**, from Michael
Azzi's [*Pixel Logic*](https://www.pixellogicbook.com/):

> "Swoopers from Super Mario World (SNES) are bats. However their nose was
> coloured orange, which makes it look like a bird with a beak."

**An orange protrusion in the head position converts a bat into a bird.** Not
silhouette — colour, on the beak. Corroborated by practice: Minecraft gives the
chicken's beak its own 4×2×2 box, Mega Voxels' bird tutorials give the beak its
own dedicated hue (`#EA951C`), and shipped 16×16 sprite packs are "blue body,
**yellow beak**, reduces down to 3 colours nicely". **That is why `beak_horn` is
a proposed material — and why, in the end, **twelve of the twenty species carry
a bright bill** (eight yellow, three orange, one white), three more carry a grey
one, and only two use the neutral horn the material is named for.**

Also from Pixel Logic: "your audience identifies faces by looking for eyes; they
are generally the first thing people notice" — but "sometimes there is no space
for eyes", and then "focus on the shadows cast on the face to create the eye
area". At 25 voxels there IS space, which is why `bird.eye` exists and why the
contrast partner beside the pupil is chosen from the species' own palette
rather than fixed.

Shipped sprite sizes cluster at **two values with nothing between**: 16×16 for
background birds and 64×64 for species work. Mega Voxels' tutorials all specify
**32×32**. MagicaVoxel's default matrix is 40³.

### Voxel practice: an honest empty result

**The published voxel-art literature on birds is close to empty.** No tutorial
found anywhere discusses folded-versus-spread wings, wing thickness, or a
minimum voxel count for a bird. Five voxel bird asset packs were checked and
**none publishes grid dimensions**. Mojang's geometry files are the only hard
data that exists. One soft corroboration: Voxel Tiny Birds ships 26 models in a
31 KB `.vox`, which at 4 bytes per voxel is **200–300 voxels per bird** — the
same order as this library's small species (244–740) and as Minecraft's parrot
(~124) and chicken (~522).

---

## 10. The pose problem, and how the prior art solves it

This is the decision birds have and fish did not, so it gets its own section.

**Three independent primary sources converge on one asset, one topology, posed
differently — never two assets.**

**Minecraft: one geometry, rotation only.**
[`parrot.entity.json`](https://raw.githubusercontent.com/Mojang/bedrock-samples/main/resource_pack/entity/parrot.entity.json)
declares `"geometry": { "default": "geometry.parrot" }` — a single entry — and
six animations. [`parrot.animation.json`](https://raw.githubusercontent.com/Mojang/bedrock-samples/main/resource_pack/animations/parrot.animation.json)
does all the work with bone rotations: the `base` pose sets the wings to
`[-40, -180, 0]`, folded flat against the flanks, and `moving` rolls them by up
to **±57.3° per radian** of `wing_flap`. The wing cube is authored **already
folded** — 1×5×3, one unit thick, sitting just outside the body.

**Infinigen: one continuous scalar.** In
[`bird.py`](https://github.com/princeton-vl/infinigen/blob/main/src/infinigen/assets/objects/creatures/bird.py)
(BSD-3), both wing factories expose a single `Extension` parameter that morphs
folded to spread, sampled `U(0, 0.05)` 80% of the time and `U(0.7, 1)` the rest.
The duck genome sets `U(0.7, 1)` when flying and **`U(0.01, 0.1)`** otherwise. A
discrete rest-rotation switch rides on top: `wing_rot = (90,0,90) if Extension >
0.5 else (90,40,90)`.

**Avian-mesh: identical topology, two rest poses.** The MIT-licensed
[avian-mesh](https://github.com/marcbadger/avian-mesh) (Badger et al., ECCV
2020) ships `bird_eccv.json` and `bird_fly_eccv.json`. Compared numerically:

| | Result |
|---|---|
| Faces identical | **yes** |
| Kinematic tree identical | **yes** |
| Skin weights identical | **yes** |
| **Rest vertex positions identical** | **NO** — mean displacement 5.36, max 16.79 |

| Rest bounding box | Perched | Flying | Ratio |
|---|---|---|---|
| **X (wing span)** | 7.17 | 32.31 | **×4.51** |
| **Y (body length)** | 20.06 | 20.86 | **×1.04** |
| Z (height) | 10.96 | 9.71 | ×0.89 |

**Two conclusions, and they point in different directions.**

First, and this is the actionable one: **spreading the wings multiplies the span
by four and a half while body length does not move.** So the voxel budget must
be keyed to body length, which is pose-invariant, and not to the bounding box,
which is not. `render.predicted_extent` does exactly that and says so.

Second: **every one of these prior arts stores exactly one topology and TWO
REST CONFIGURATIONS.** Minecraft stores one geometry plus a folded rest pose
plus rotation deltas. Infinigen stores one part plus a scalar plus a discrete
rotation switch. Avian-mesh literally ships two files whose faces and weights
are identical and whose rest vertices are not.

**A cubic voxel asset cannot store a rotation delta. The grid IS the rest
configuration.** There are no bones, there is no skin weight, and a wing that is
three voxels thick folded and one voxel thick spread is not the same set of
voxels rotated — at this resolution the rotation is not even representable. So
the voxel-space equivalent of "one mesh, two rest poses" is **one spec, two
poses**, which is what `bird.pose` is, and it costs one changed field in a
four-kilobyte JSON file rather than a second asset library.

Commercial rigs agree on the shape of the answer: the
[VFXGrace cormorant rig](https://www.vfxgrace.com/product/realistic-cormorant-3d-model-for-maya-wing-folding-rig-feather-cards-4-animations/)
ships "preset wing-folding controllers" and four animations **including
take-off** — the transition itself is authored, which is exactly the thing a
voxel grid cannot interpolate.

**And the part of "one topology, two rest poses" that two poses in two specs
does NOT give you for free.** Every prior art above stores one topology, so the
two rest poses are unarguably the same animal — same faces, same skin weights,
same bone lengths. Two specs are not the same animal by construction, and here
they were not: `pipeline.rng_for` seeded the individual from `spec.spec_hash`,
the pose is part of that hash, so `common-raven` seed 7 perched and
`common-raven` seed 7 flying came out different lengths with different markings.
The topology equivalent had to be restored deliberately, by seeding from a hash
that leaves the pose out (`spec.seed_hash`, `spec.SEED_INVARIANT`). **Anything
that adds a second authored pose to any kind here inherits the same problem**,
and the fix generalises: the field goes in `SEED_INVARIANT`, normalised to its
default rather than deleted, so nothing that never authored it reseeds.

---

## 11. Existing procedural bird generators

**[Infinigen](https://github.com/princeton-vl/infinigen)** (BSD-3, CVPR 2023) is
the closest prior art and the only substantial open-source parametric bird
geometry generator that exists. Two factories, `BirdFactory` (`duck_genome`) and
`FlyingBirdFactory`:

| | Bird | Flying bird |
|---|---|---|
| Body length | 0.85 × N(1,0.2) × N(1,0.2,3) | 0.95 × N(1,0.05,3) |
| Head length | 0.35 (**41% of body**) | 0.30 (**32% of body**) |
| Eye radius | N(0.03, 0.005) | N(0.02, 0.005) |
| Eye coordinate | (0.65, −0.32, 0.95) | same |
| Wing length | body × 0.5 × clip_gaussian(1.2, 0.7, 0.5, 2.5) | body × clip_gaussian(1.0,0.2,0.6,1.5) × 0.8 |
| Tail feathers | int(N(16, 3)) | int(N(16, 3)) |
| Leg length | body × 0.5 × N(1, 0.05) | same |

**How it generates variety, and this is the transferable idea:** it does not
parameterise a bird abstractly. It **convex-blends a small library of real
species archetypes** — three body NURBS grids (`body_bird_duck.npy`,
`body_bird_gull.npy`, `body_bird_robin.npy`), each **9 cross-sections along the
body × 8 points around**, and four beak templates (`normal`, `duck`, `eagle`,
`short`) of about 28 scalar parameters each. The eagle beak is distinguished
almost entirely by `hook_scale_x: -1.0` and `hook_scale_z: -0.8`.

That body representation is a **lofted generalized cylinder** — a spine with
per-station cross-section radii — which is precisely what `bird._body` is.

Its measured normalised radius profiles along the body:

| | t=0 | 0.25 | 0.48 | 0.7–0.86 | 0.9 | 1.0 |
|---|---|---|---|---|---|---|
| duck | 0 | 0.61 | 0.95 | **1.00** | 0.31 | 0 |
| gull | 0 | 0.55 | 0.98 | **1.00** | 0.39 | 0 |
| robin | 0 | 0.77 | **1.00** | 1.00 | 0.66 | 0 |

**Read with a caveat: which end of `t` is the head is not stated in the files
and could not be established.** Under either reading the peak sits between 48%
and 86% of the body from one end, which brackets this generator's
`bird.chest_at` of 0.30–0.45 measured from the breast. Their profile also runs
to zero at BOTH ends, because their body includes the neck taper; this one
stops at a blunt breast and hands the neck to a separate part, which is why
`bird.breast` bottoms out at 0.20 rather than at 0.

**[avian-mesh](https://github.com/marcbadger/avian-mesh)** (MIT) — a
skinned linear bird model, **3,932 vertices, 5,684 faces, 25 joints, 12
keypoints**, parameterised by global pose, body pose, bone length and scale.
The two rest poses are the measurement in §10.

**Explicit negative results, worth as much as the positives:**

- **No academic literature on procedural bird *geometry* generation.** An arXiv
  full-text search for `procedural` + `birds` returns Angry Birds level
  generation and bird-audio classification and nothing else. OpenAlex likewise.
- **No open-source voxel bird generator exists.** GitHub searches for `voxel
  bird generator` and `procedural creature generator voxel` return **zero
  results**.
- **No minimum voxel count at which a bird stops reading has ever been
  published.** Section 12 measures one here instead.

---

## 12. The lattice

Not from the literature — measured here, with `tools/birdprobe.py --lattice`.
The terrain is 10 cm, every plant in this library is 5 cm, and a fish is 1 cm.
A bird is authored at **1 cm**, and the reason is not cost.

Measured on seed 1, features rather than voxel counts, because the lattice
question is entirely about whether the features still exist:

| Species | Lattice | Length | Bill | Eye | Tail | Neck | Crest | Voxels |
|---|---|---|---|---|---|---|---|---|
| european-robin | 1 cm | 19 | 2 | **2** | 7 | 1 | 1.0 | 445 |
| | 2 cm | 10 | 3 | 1 | 5 | 1 | 1.0 | 85 |
| | 5 cm | **7** | 3 | 1 | 5 | 0 | 1.0 | 41 |
| eurasian-hoopoe | 1 cm | 26 | 5 | **2** | 9 | 5 | **9.0** | 435 |
| | 2 cm | 14 | 3 | 2 | 5 | 1 | 4.5 | 105 |
| | 5 cm | **9** | 2 | 1 | — | **0** | 2.0 | 35 |
| grey-heron | 1 cm | 80 | 16 | **2** | 3 | **17** | 3.0 | 12,426 |
| | 2 cm | 41 | 9 | 1 | 2 | 9 | 1.0 | 1,646 |
| | 5 cm | 17 | 4 | 1 | 1 | 3 | 1.0 | 167 |
| golden-eagle | 1 cm | 83 | 8 | **2** | 15 | 3 | 5.5 | 28,355 |
| | 2 cm | 42 | 4 | 2 | 8 | 3 | 4.0 | 6,790 |
| | 5 cm | 17 | **0** | 1 | 4 | 3 | 0.0 | 1,189 |

*(Run with the eye and bill forced to colours nothing else on the bird wears,
because nine of the twenty species paint their eye in a colour something else
also uses; and with individual variation pinned. The 5 cm hoopoe's tail column
is dashed because the whole animal is nine voxels and the tail measurement has
nothing left to measure against.)*

Read the eye and the bill columns.

**At 1 cm every species has a two-voxel eye — a pupil and its contrast partner.
At 5 cm every species has one voxel and no partner**, which is a dark speck on
a dark head, and §8 of this document is about why value contrast is the thing
that makes a mark read at all. **At 5 cm the bill is gone entirely on nine of
the twenty species**, including the golden eagle, whose hooked bill is most of
what says raptor.

At 2 cm a robin is **ten voxels long** — below Minecraft's own shipped parrot
at 11 — its neck is one voxel and its eye has lost its partner. At 5 cm it is
**seven**.

**Only 1 cm carries a bill, a two-voxel eye, a neck and a crest across the whole
set.**

Cost is not the constraint at any of these sizes, and the margin is much wider
than it looks. Measured with variation pinned, the twenty species run from
**334 voxels** (song-thrush) to **28,355** (golden-eagle, a 2.1 m wingspan),
and all twenty together are **87,460**. One `temperate-oak` at its authored
5 cm is **1,065,343**. So a flock of forty song-thrushes is **1.25% of one
tree**, and the largest bird in the library is **2.7%** of one. 1 cm nests
**10:1** in the terrain lattice and **5:1** in the 5 cm asset lattice, both
whole numbers, so a bird placed at a fine-lattice coordinate lands exactly on
it.

What 1 cm does **not** buy is a life-sized small bird. A goldcrest is 9 cm and a
wren is 10, which is nine or ten voxels. **Four species in this library are
authored above life size to clear the owner's 20 cm floor** — european-robin
(24 cm against a real 14), great-tit (24 against 14), common-kingfisher (20
against 17) and barn-swallow (26 against 17–19) — and each says so in its own
`notes`, because a note is what stops the next person "correcting" it back.

**Three of those four were enlarged FURTHER than the floor, for a reason the
floor does not capture.** A perched songbird is authored at 36–42 degrees
nose-up, and 20 cm of bird at 42 degrees projects onto sixteen voxels of length.
`tools/birdprobe.py --read` measures the animal's longest axis rather than its x
extent for exactly this reason, and it flagged the robin, the great tit and the
barn swallow as SHORT at 20 and 22 cm and passes all three at 24 and 26.

---

## 13. Datasets and licences

The fish work rejected every dataset it looked at on licence grounds. **The
bird situation is materially better and it is worth stating plainly, because
the conclusion is the opposite one.**

| Dataset | What | Size | Licence | **Commercial** |
|---|---|---|---|---|
| **AVONET** | 11 morphological traits, all birds | 90,020 individuals / 11,009 species | **CC BY 4.0** | **YES** |
| **EltonTraits 1.0** | diet, foraging stratum, mass | 9,993 birds | **CC BY 4.0** | **YES** |
| Amniote Life-History DB | 29 life-history params | 9,802 birds | CC BY 4.0 | YES |
| eBird via **GBIF** | occurrences | 1.78 bn records | CC BY 4.0 | YES |
| Sheard et al. Global-HWI | per-species HWI | 10,338 spp | **"Other (Open)", no LICENSE file** | **AMBIGUOUS** |
| **eBird API** | same data, live | — | explicit non-commercial | **NO** |
| Macaulay Library | media | — | all rights reserved | **NO** |
| **Cornell Birds of the World** | species accounts | — | subscription, commercial use prohibited | **NO** |
| **HBW** (Lynx) | measurement tables | 17 vols | all rights reserved | **NO** |
| BTO BirdFacts | UK biometrics | ~600 spp | **no licence statement found** | **AMBIGUOUS** |
| PhyloPic | silhouettes | 12,893 | **per image**: CC0 / PDM / CC BY / BY-SA / BY-NC | **per image** |
| CUB-200-2011 | 11,788 photos, 312 attributes | 200 spp | non-commercial research only | **NO** |

**AVONET was checked in three independent places for the self-contradiction
that sank FishShapes, and there is none.** The
[figshare API](https://api.figshare.com/v2/articles/16586228) returns
`"license": {"value": 1, "name": "CC BY 4.0"}`; the paper (Tobias et al. 2022,
*Ecology Letters* 25:581–597, [DOI](https://doi.org/10.1111/ele.13898), read via
the [Glasgow repository copy](https://eprints.gla.ac.uk/266253/)) states "this
is an open access article under the terms of the Creative Commons Attribution
License"; and [Open Traits Network](https://opentraits.org/datasets/avonet.html)
records it as CC BY 4.0 independently. CC BY 4.0 §4 explicitly grants the right
to "extract, reuse, reproduce, and Share all or a substantial portion of the
contents of the database", which answers the sui-generis database-right question
that is the usual trap with trait tables.

**Two traps found while checking:**

- The **Zenodo mirror of EltonTraits** ([record 20244652](https://zenodo.org/records/20244652))
  shows **no licence field at all** and marks its files "Restricted". The real
  licence is one level up, at
  [esapubs.org/archive/copyright.htm](https://esapubs.org/archive/copyright.htm):
  "Ecological Archives are covered under the Creative Commons Attribution (CC BY
  4.0) license." Do not use the mirror as licence evidence.
- **Sheard et al.'s Global-HWI deposit is the fish-dataset trap in bird form**:
  Zenodo says "Other (Open) — no further description", and the GitHub repo
  returns `"license": null` with no LICENSE file. **Do not ship it.** It is not
  needed: AVONET carries `Hand-Wing.Index` for all 11,009 species under CC BY
  4.0, and its own metadata notes its values differ from Sheard's "because of
  much higher sampling of individuals in some species".

### So why is AVONET not vendored?

**Because twenty species do not need eleven thousand rows.** The generator
takes a species name and produces a bird; the table it needs is twenty entries
of half a dozen ratios each, and those are typed into
`tools/seed_birds.py` and `forge/language.py` by hand from published medians.
Embedding 90,020 rows to use twenty of them would add a permanent attribution
obligation, a data-versioning question and a licence header to every build of
the game, in exchange for nothing the hand-typed table does not already do.

**The medians themselves are facts and carry no licence.** The attribution is
given anyway, in `tools/seed_birds.py` and here, because it costs one line and
CC BY asks for it.

**If a future version needs two hundred species rather than twenty, vendor
AVONET.** That is the point at which the trade flips, the licence permits it,
and the only work is a citation in the credits. Filter `Inference == NO` if you
want measured ground truth rather than values inferred from relatives.

**PhyloPic, if silhouettes are ever wanted:** whitelist CC0, Public Domain Mark
and CC BY only, machine-checked per image via
[api.phylopic.org](https://api.phylopic.org/images/), and store the licence URL
and attribution string alongside every asset. New submissions are restricted to
those three licences, so the NC and SA problem is confined to the legacy corpus.

---

## 14. What was adopted, and what was rejected

### Adopted

**A jointed layout, not a loft.** A fish is one solid whose cross-section
changes along a straight axis. A bird is a body, a neck, a head, a bill, a tail
and two wings at angles to each other, and §1 says the angles are what identify
it. So `forge/bird.py` solves a layout in float voxel coordinates first, sizes
the grid from that layout's own bounding box, and draws six parts.

**Proportion as five normalised shares** (§1, §5). Bill, head, neck, body and
tail, summing to one, because the cue birders name first is internal
proportion and proportion is a set of shares. Normalising is what keeps
`bird.length_m` meaning what it says.

**Posture as one number** (§1). A duck at 4 degrees and a woodpecker at 68 are
the same generator.

**Savile's four planforms as a choice, with aspect ratio as a separate
slider** (§2, §3). The four differ in how chord is DISTRIBUTED; aspect ratio
sets how much there is. Keeping them separate is what lets a buzzard (5.6), an
accipiter (6.2) and a falcon (7.9) be three different birds.

**The 0.85 body-strip correction** (§3), because the published aspect ratios
include the body between the wings and this generator draws only the wings.

**Bill depth ÷ length and width ÷ depth as the two bill axes** (§6). They are
the first principal component of a 2,028-species 3D morphospace and an
independent second axis, and between them they separate a finch from a warbler
from a heron from a duck.

**Tail shape as one signed length profile across the fan** (§7). Seven
field-guide names out of four lines, because that is what the taxonomy actually
is.

**Three marking regions, not one** (§8). A fish gets one mark because its
stripe and its bars are drawn on the same twelve-voxel flank. A bird's cap, its
wing bar and its breast streaking are on disjoint voxels, and the head gets
nearly three times the expert vocabulary of anywhere else, so it gets its own
marking colour too.

**One hard countershading boundary rather than a gradient** (§8). Cuthill's
result is that a sharp transition is optimal under direct sun and worth nothing
under diffuse light; a hard boundary is also what every field guide draws.

**Colour deliberately over-weighted toward the rare hues** (§8). Melanin is 74%
of the area and 7% of the gamut. A naturalistic palette is browns and greys.

**A prominent, brightly coloured bill** (§9). An orange nose turns a bat into a
bird, and it is the cheapest identifying feature available.

**Pose as an authored parameter with two settings** (§10). Every prior art
stores one topology and two rest configurations; a voxel grid IS its rest
configuration, so two poses means two specs.

**The voxel budget keyed to body length, not to the bounding box** (§10). Span
moves by 4.51× between poses and body length by 1.04×.

**Hand-typed species medians rather than an embedded dataset** (§13) — for
size, not for licence, which is a different reason from the fish work's and is
stated as such.

### Rejected, and why

**Vendoring AVONET.** Not on licence — it is CC BY 4.0 and usable. On
proportion: 90,020 rows to serve twenty species, in exchange for a permanent
attribution obligation and a data-versioning problem. Revisit at 200 species.
**The Sheard HWI deposit is rejected on licence** and does not need to be used.

**Deriving markings from the biome weights.** The obvious feature — barred
birds in forest, streaked birds in the open — is **not supported by the only
global test of it**: McFadden R² median **0.0065** over 2,756 species (§8). It
would have been a plausible-sounding invention.

**Reaction–diffusion for plumage.** The developmental biology is real and
unusually well characterised (§8), and it is the wrong level. Prum &
Williamson's model patterns ONE FEATHER; the marks that read at 25 voxels are
tract-level, and the tract level is Haupaix's somite prepattern, which is not a
diffusion process at all but an instruction. So the marks here are parametric
regions, exactly as the biology says they are, and the one that IS noise —
`speckled` — is quantile-thresholded so its coverage means what it says.

**Species-diagnostic detail of any kind.** Machine species accuracy is 31.6% at
25 px against 51% at 100 px, and human basic-level recognition is near-saturated
at 32 px (§9). The information is not there. The twenty species are built to be
distinguishable from EACH OTHER, which is a much weaker and much more
achievable requirement.

**Fitting the bill to diet automatically.** Diet explains under 12% of beak
shape variation, and beak alone predicts trophic niche at 60% where the full
trait vector reaches 85% (§6). A diet-to-bill mapping would look principled and
produce birds that read as wrong.

**Individual tail feathers.** Twelve rectrices is the modal count and rectrix
number explains r² = 0.15 of anything (§7). At a tail six to forty voxels long,
drawing twelve feathers is drawing noise.

**A continuous folded-to-spread wing parameter**, which is what Infinigen uses.
It works for a mesh because a mesh interpolates. In cubic voxels the two ends
are a three-voxel bulge along the flank and a one-voxel plate reaching thirty
voxels out; there is no shape between them that is a bird, and the intermediate
values would all be wrong. `bird.pose` is a choice of two (§10).

**A third pose.** "Gliding" is `flying` with a different sweep and dihedral, and
a choice that duplicates a slider position is a choice that silently ignores it.
The language box has "gliding" as a phrase that sets those two sliders.

**Blending species archetypes**, which is Infinigen's whole variety mechanism.
It needs a template library and a temperature per species to stop it producing
nonsense — Infinigen pins its own eel to 0.01 and its puffer to 0.001 for
exactly that reason — and this generator has explicit parameters where they have
latent weights. A blended heron-and-duck is not an intermediate animal.

**Meshing and then voxelising.** Conservative voxelisation over-fills by one to
two voxels everywhere and destroys thin plates, which here is every wing, every
tail and every leg. Same conclusion the tree and fish generators reached, for
the same reason.

**A separate colour system for birds.** ADR-0008 gives every voxel face one flat
colour from the engine's material palette. Birds need colours that palette does
not have, and the answer is eleven more materials, not a second path. See
`docs/bird-colour-proposal.md`.

**The 5 cm asset lattice, and the 2 cm one.** Measured here rather than
argued (§12): at 5 cm every one of the twenty species loses its bill's contrast
partner and **nine of the twenty lose the bill entirely**, the eagle included;
a robin is seven voxels long. At 2 cm a robin is ten, which is shorter than
Minecraft's shipped parrot at eleven, and its neck is one voxel. Cost was never
the argument — all twenty birds together are 87,460 voxels against 1,065,343
for one oak.

**More than about five bars on a small bird.** `bird.mark_count` used to run to
24 and it was measured as DEAD across its whole range: a 20 cm bird has a body
eight voxels long, so 24 bars is a period of a third of a voxel and one bar and
twenty-four come out identical. The slider was not dead, it was SATURATED. The
ceiling is 10 now, and even 10 only reads on the largest species — which is the
same two-on-two-off floor the fish work measured for bars, arrived at
independently.
