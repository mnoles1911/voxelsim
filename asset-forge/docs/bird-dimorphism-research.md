# Male and female birds: what is worth drawing at 1 cm, and what is not

Companion to `docs/bird-shape-research.md` and `docs/bird-colour-proposal.md`,
and the bird half of what `docs/marine-marking-research.md` did for fish.
It answers one question per species: **is there a difference between the sexes
big enough to survive this lattice, and if so what carries it?**

Eight of the twenty carry a difference. **Twelve are an honest null** — 1.00 on
both ratios, `same` on the plumage, and `python tools/birdprobe.py --sex`
measures them moving no geometry at all. (Three of the twelve show a few per
cent of *repainted area* between a male and a female that differ in no authored
value whatsoever. That is their marking phase, not a difference; the probe
prints a measured control beside it, and `m_recolour` explains why a 314-voxel
song thrush does it.)

---

## The one sentence that decides the whole design

**In fish, sexual difference is size. In birds it is colour.**

The fish work got twenty-three species out of three ratios and a square root,
because what separates a bull orca from a cow is the height of a fin. Nothing
of the sort works here. The largest single difference between two animals of
one species anywhere in this library is a mallard drake against a hen — bottle
green head, white collar, grey body, yellow bill, against uniform mottled
brown — and **not one voxel of it is a proportion**.

So `bird.sex` drives two mechanisms rather than one:

| | mechanism | authored as | how `unsexed` behaves |
|---|---|---|---|
| size | `bird.sex_length`, `bird.sex_tail` | a male:female ratio | the geometric mean of the two — genuinely neither sex |
| colour | `bird.sex_plumage` + ten `alt` rows | one sex's colours, plus what the other swaps | **one of the two sexes**, and the spec says which |

The size half is `fish.sex`'s rule verbatim: the authored number is the species
average, the male gets `sqrt(r)` and the female `1/sqrt(r)`, male divided by
female is exactly `r`, and neither sex is the default.

The colour half cannot be. **There is no average of a green head and a brown
one.** Halfway between them is a colour no duck has ever worn. So a dimorphic
species is authored as one sex, `bird.sex_plumage` names which, and `unsexed`
draws that one. That is a real limitation and it is stated in four places — the
parameter help, `bird._alt`, this document, and the probe's own output, which
prints the list:

```
  UNSEXED IS ONE OF THE SEXES on these, because colour has no average:
    common-kestrel (male)
    great-spotted-woodpecker (male)
    mallard-duck (male)
    rock-ptarmigan (male)
```

### Why not pick a sex from the seed instead?

Considered and rejected. Having `unsexed` resolve to drake or hen depending on
the individual's seed would give a flock a natural mix, which is what a world
generator actually wants. It was rejected because it fights the size half: the
ratios would still hand `unsexed` the geometric mean, so an unsexed mallard
could come out drake-coloured at hen size — a chimera, and a chimera that no
measurement in the probe would flag. Mixing a flock is the spawner's job, and
the spawner can ask for a sex.

---

## The rule every number below is judged against

A difference has to move **two voxels**, or it reads as a mistake rather than as
a difference. That is the same threshold the aquatic work used. What is new
here is that it has to be applied in two units at once, and the great spotted
woodpecker is why. Its entire published difference is a crimson nape patch that
measures **four voxels on a 352-voxel bird** — 1.1% of the animal. A per-cent
gate calls the most-quoted field mark for that species a no-op; a two-voxel
gate alone would pass a two-voxel change on a 28,355-voxel golden eagle. So
`birdprobe --sex` gates on both.

### What two voxels costs, per species

Measured, not estimated: this is the smallest male:female **length** ratio that
moves the built animal by two voxels, taken off `pipeline.build` at
`variation.amount = 0` with the pose pinned perched.

| species | length, voxels | smallest ratio worth authoring |
|---|---|---|
| golden-eagle | 83 | 1.024 |
| grey-heron | 80 | 1.025 |
| scarlet-macaw | 76 | 1.027 |
| herring-gull | 59 | 1.034 |
| common-raven | 58 | 1.035 |
| mallard-duck | 55 | 1.037 |
| common-buzzard | 51 | 1.040 |
| rock-ptarmigan | 35 | 1.059 |
| common-kestrel, tawny-owl | 33 | 1.062 |
| rock-pigeon | 31 | 1.067 |
| eurasian-jay | 30 | 1.069 |
| eurasian-hoopoe | 26 | 1.080 |
| barn-swallow | 24 | 1.087 |
| great-tit | 21 | 1.100 |
| european-robin, song-thrush | 20 | 1.105 |
| common-starling, common-kingfisher | 19 | 1.111 |
| great-spotted-woodpecker | 13 | 1.166 |

**This table is most of the answer.** Linear sexual size dimorphism in
passerines runs 2–4%. Every songbird in this set needs 8–17% before the
difference is a voxel and a half. That is not a judgement call, it is
arithmetic, and it is why fourteen species get 1.00 on size without any
argument about ornithology.

(The woodpecker's 13 voxels is not a typo. It is authored at 68 degrees
nose-up, clinging to a trunk, so 23 cm of bird projects onto 13 columns of x
and 16 of z. `docs/bird-shape-research.md` records the same surprise.)

---

## ADOPT

### Size — `bird.sex_length`

**1. golden-eagle 0.93.** Primary, and unusually well measured — Bortolotti's
Table 1 is 50 adults with standard deviations and ranges, and five linear
characters all land on the same ratio.

| character (adults) | male | female | M:F |
|---|---|---|---|
| wing chord | 595.0 ± 15.11 (569–619), n=23 | 640.4 ± 19.75 (601–674), n=27 | **0.929** |
| eighth primary | 437.0 ± 12.54, n=23 | 466.0 ± 13.80, n=26 | 0.938 |
| first secondary | 359.1 ± 10.48, n=19 | 384.0 ± 13.10, n=25 | 0.935 |
| tail length | 286.5 ± 10.03, n=22 | 307.2 ± 9.69, n=24 | 0.933 |
| culmen length | 40.55 ± 1.44, n=23 | 44.21 ± 1.66, n=27 | 0.917 |
| hallux claw | 49.35 ± 1.69, n=23 | 55.67 ± 2.70, n=26 | 0.887 |

> "TABLE 1. Sex and age class variation in size of Golden Eagles." / "Wing chord Adult 23 595.0 ± 15.11 (569-619)" / "640.4 ± 19.75 (601-674)" / "All measurements are in millimeters."
> — Bortolotti, *Age and Sex Size Variation in Golden Eagles*, J. Field Ornithol. 55(1), 1984, <https://digitalcommons.usf.edu/jfo/vol55/iss1/6>

Corroborated independently by wingspan:
> "The male of this subspecies has a wingspan of 1.89 to 2.15 m, with an average of 2.02 m" / "the female's typical wingspan range is 2.12 to 2.2 m, with an average of 2.16 m"
> — <https://en.wikipedia.org/wiki/Golden_eagle> — 2.02 / 2.16 = 0.935.

**Authored 0.93. Measured: 85 voxels female against 79 male, six voxels.**

A note on why this is not a mass ratio. The same Wikipedia page gives 3.69 kg
against 5.17 kg, a ratio of 0.714 whose cube root is **0.894**, and Bortolotti's
own tail and wing figures say 0.93. Authoring the mass-derived number would have
drawn an eagle half again as dimorphic as she is. Where a source publishes a
linear measurement and a mass, the linear one wins; where only a mass exists,
the cube root is used and is labelled DERIVED at every site.

Bortolotti's tail ratio, 0.933, is also why the eagle carries no `sex_tail`:
her tail is longer in exactly the proportion the rest of her is, so
`sex_length` already draws it.

**2. common-buzzard 0.95, and this is the least certain number in the set.**
The source states it linearly, which is rare and worth quoting:
> "Females average about 2–7% larger than males linearly and weigh about 15% more."
> — <https://en.wikipedia.org/wiki/Common_buzzard>

Wing chords on the same page: male 350–418 mm (midpoint 384), female 374–432 mm
(midpoint 403) — a ratio of 0.953. A second compilation reached during this work
gives 0.972. So the honest position is a range of **0.953 to 0.972**, and 0.95
sits just outside its low end. **Authored 0.95. Measured: 52 against 49, three
voxels.** At 0.96 it would move 2.1 voxels and at 0.972 about 1.4 — under the
floor — so the choice between "author the low end" and "author nothing" is
being made on a source that cannot resolve it. Every open primary tried
(Zuberogoitia 2005, and the 2015 morphometric-sexing paper) is behind a 403.
This is the figure most likely to be wrong.

**3. herring-gull 1.06 — corrected during authoring, from 1.09.** Worth
recording because the correction is the whole method working. It was first set
from a field-guide length range:
> "The male European herring gull is 60–67 cm long" / "the female is 55–62 cm"
> — <https://en.wikipedia.org/wiki/European_herring_gull> — midpoints 63.5 and 58.5, ratio 1.085.

Two primary papers then put it lower, and agree with each other.

> "Using the cube root of the body weights (to facilitate comparisons with linear measurements) the males averaged from 1.04 to 1.09 times as large as the females (Table 1)."
> and, for *L. argentatus* specifically, Table 1's cube-root column: Murman coast 1.054 (162♂/94♀), Norway 1.076 (102/89), Mellum 1.067 (80/80).
> — Ingolfsson, *The Behaviour of Gulls / Sexual Dimorphism of Large Gulls*, Auk 86:732–737, 1969, <https://digitalcommons.usf.edu/auk/vol86/iss4/12>

> "Wing length BM 414.54 ± 24.79 365-440" / "BF 396.06 ± 20.19 372-423" / "NM 440.37 ± 22.40 412-470" / "NF 416.58 ± 26.01 388-445" / "BM, British male (98), BF, British female (108), NM, Newfoundland male (180), NF, Newfoundland female (78)"
> — Threlfall & Jewer, Auk 95, October 1978, <https://digitalcommons.usf.edu/auk>

Wing 1.047 and 1.057; tarsus 1.077 and 1.099; culmen depth 1.101 and 1.096.
Taking the two cube-root-of-mass figures and the wings together, **1.06** is the
central estimate. **Measured: 57 voxels female against 61 male, four voxels.**

⚠️ **AND INGOLFSSON'S TABLE 1 IS THE TRAP THIS PROJECT ALREADY FELL INTO ONCE.**
Beside that cube-root column sit five more headed *culmen, bill depth, tarsus,
wing, tail*, printing values like `102.1  103.2  100.7  98.1  99.6`. They read
exactly like millimetres and they are not:
> "Degree of sexual dimorphism is shown as the ratio of male to female measurements expressed as a percentage of the cube root of male over female weights."

They are **normalised indices**. 98.1 for the wing does not mean females have
longer wings; it means a male's wing is 1.9% shorter than his overall size
would predict. Anyone grepping that paper for gull measurements pulls out
plausible two- and three-digit numbers that mean nothing physical — which is
precisely the shape of the orca eye patch this project documented as "21.8 ×
5.9 cm, measured" three times over when the figures were two dimensionless
indices from an unrelated paper.

### Tail — `bird.sex_tail`

**4. barn-swallow 1.20.** The only tail ornament in the set and the only
species whose difference is a proportion rather than a colour.
> "Males in northern Europe have longer tails than those further south, whereas in Spain the male's tail streamers are only 5% longer than the female's; in Finland, the difference is 20%."
> — <https://en.wikipedia.org/wiki/Barn_swallow>

Corroborated on which feather carries it, which matters:
> "Feather length differed significantly between feather types and sexes, and the significant sex × feather type interaction indicates that the difference between sexes was non-significant for P1 and Ta5, while Ta6 was significantly longer in males than in females"
> — *Sexual Dimorphism and Population Differences in Structural Properties of Barn Swallow Wing and Tail Feathers*, PLoS One 2015, PMC4482263, <https://pmc.ncbi.nlm.nih.gov/articles/PMC4482263/>

So it is the outermost rectrix and nothing else — the wing is not involved,
which is why this is a separate ratio from `sex_length` and why `sex_length`
stays at 1.00 for this species.

**Authored 1.20, the northern figure, and this is the marginal case in the
library.** Measured perched: tail 11 columns against 13, and overall length 26
against 28. The Spanish figure of 1.05 moves the tail by a fifth of a voxel and
would have been a no-op. The species is also authored at 26 cm against a real
17–19, deliberately, so that its fork has voxels to be a fork in — which is
what buys the streamer its two columns.

### Plumage — `bird.sex_plumage` and the `alt` rows

**5. mallard-duck, authored male.** The flagship, and the reason the whole
plumage mechanism exists.
> "The breeding male mallard is unmistakable, with a glossy bottle-green head" / "a white collar that demarcates the head and neck from the purple-tinged brown breast" / "grey-brown wings, and a pale grey belly" / "The bill of the male is a yellowish-orange tipped with black"
> "The female mallard is predominantly mottled, with each individual feather showing sharp contrast from buff to very dark brown" and has "buff cheeks, eyebrow, throat, and neck, with a darker crown and eye-stripe"
> "The bill of the male is a yellowish-orange tipped with black, with that of the female generally darker and ranging from black to mottled orange and brown."
> — <https://en.wikipedia.org/wiki/Mallard>

Six `alt` rows: head → `plume_buff`, back → `skin_brown`, belly →
`plume_buff`, wing → `skin_brown`, bill → `beak_horn`, head marking → `none`
(the white collar goes).

**The speculum is deliberately NOT swapped, and the source is the reason:**
> "Both male and female mallards have distinct iridescent purple-blue speculum feathers edged with white"

It is the one mark on the bird identical in both sexes, which is exactly why it
is the field mark you identify a hen by. **Measured: 95.9% of the bird
repainted, 6,587 voxels** — the largest sexual difference in the library by a
factor of forty-five over the next one.

**6. common-kestrel, authored male.**
> "the male having fewer black spots and streaks, as well as a blue-grey cap and tail. The tail is brown with black bars in females"
> — <https://en.wikipedia.org/wiki/Common_kestrel>

Two `alt` rows: head → `plume_rufous`, body marking → `barred`. The marking is
the interesting half: she is barred where he is spotted, **in the same dark
brown**, so no material swap could have said it. **Measured: 22.3% repainted,
146 voxels.**

**7. great-spotted-woodpecker, authored male.**
> "Males have a crimson patch on the nape, which is absent from the otherwise similar females."
> — <https://en.wikipedia.org/wiki/Great_spotted_woodpecker>

One `alt` row: head marking → `none`. **Measured: 4 voxels, 1.1% of the bird**
— the smallest adopted difference in the library, and the reason the probe
gates on voxels as well as per cent.

**8. rock-ptarmigan, authored male.**
> "Apart from the red eye combs, male rock ptarmigans have no 'distinct' plumage"
> and, on the black loral stripe, males have "black lores, absent in the willow ptarmigan, though they are also absent in female rock ptarmigan"
> — <https://en.wikipedia.org/wiki/Rock_ptarmigan>

One `alt` row: head marking → `none`, dropping the crimson comb. **Measured: 20
voxels, 0.6% of a 3,424-voxel bird**, and it reads only because it is the only
saturated thing on an otherwise white animal.

This is the weakest-sourced adoption in the set. Wikipedia is the only source
reached for it, its plumage section is thin, and the two statements above are
the whole of what was found. It is flagged here rather than dressed up.

---

## REJECT — with the number that decided it

Every one of these is a real, documented difference between the sexes. They are
rejected because of what they measure at 1 cm, not because they are not real.

**1. tawny-owl size — 1.6 voxels.**
> "This species is sexually dimorphic; the female is much larger than the male, 5% longer and more than 25% heavier."
> — <https://en.wikipedia.org/wiki/Tawny_owl>

5% longer gives a male:female ratio of 0.952. On a 33-voxel owl that moves
`|sqrt(0.952) - 1/sqrt(0.952)| x 33 = 1.62` voxels, against a floor of two.
Authored 1.00. This is the closest miss in the set.

**2. common-kestrel size — 1.9 voxels, DERIVED.**
> "the adult male weighing 136–252 g, around 155 g on average; the adult female weighs 154–314 g, around 184 g on average"
> — <https://en.wikipedia.org/wiki/Common_kestrel>

No linear measurement was found for this species, so: 155/184 = 0.842, cube
root **0.945 (DERIVED, not measured)**. On 33 voxels that is 1.87 voxels.
Under the floor, and doubly so because it rests on a cube root rather than on a
ruler. Authored 1.00 — the kestrel is carried entirely by its colour.

**3. scarlet-macaw size — 1.025 measured against a 1.027 threshold, and not
statistically significant anyway.** The closest a rejection has come to being an
adoption, and the only one where the source itself declines to claim the
difference. Wiedenfeld measured 31 wild-taken museum specimens, "All
measurements are in mm":

> "TABLE 1. Measurements for four characters in Ara macao, all in mm." — *A. m. macao*, wing chord: ♂ 394.0 (14.1), n=10; ♀ 384.3 (13.9), n=12
> — Wiedenfeld, *A New Subspecies of Scarlet Macaw and Its Status and Conservation*, Ornitología Neotropical 5:99–104, 1994, <https://digitalcommons.usf.edu/ornitologia_neotropical/vol5/iss2/4>

394.0 / 384.3 = 1.0252. The macaw is the third longest bird in the library at 76
voxels and still needs **1.027** to move two of them. The standard deviations
(14.1 and 13.9) are larger than the difference between the means, and the paper
reports the comparison as non-significant. Authored 1.00.

A tail ratio was looked for and **NOT FOUND**: Wiedenfeld measured maxilla,
mandible, longest toe and wing chord, and no tail. The only tail claim reached
anywhere is hedged and unquantified ("tail feathers of males *may be* longer"),
which is not a number.

**4. Every passerine's size — 8 to 17% needed, 2 to 4% available.** Raven, jay,
robin, great tit, song thrush, starling, hoopoe, swallow. See the voxel table
above. No sex-specific linear measurement was found for the common raven on any
source reached (Wikipedia's account gives regional means and no sexes), so this
is a rejection on the threshold rather than on a number, and it is recorded as
such.

**5. common-kingfisher bill colour — the bill is one voxel deep.**
> "The female is identical in appearance to the male except that her lower mandible is orange-red with a black tip."
> — <https://en.wikipedia.org/wiki/Common_kingfisher>

This is a genuinely diagnostic, genuinely visible field mark, and it cannot be
drawn. Measured on the built asset: the kingfisher's bill is **6 columns long
and 1 voxel deep at the base**. A lower mandible is half of one voxel. Worse,
`forge/bird.py::_bill` draws the bill as a single tapered solid with no upper
and lower half to colour separately, and `materials.bird_bill` paints the legs
with it. Drawing this needs new geometry, not a new parameter.

**6. common-starling — three differences, all under one voxel.**
> "females have lemon yellow beaks with pink bases while males have yellow bills with blue-grey bases"
> "The throat feathers of males are long and loose and are used in display while those of females are smaller and more pointed."
> "They can usually be sexed by the colour of the irises, rich brown in males, mouse-brown or grey in females."
> — <https://en.wikipedia.org/wiki/Common_starling>

The starling's whole bill is **3 columns and 1 voxel deep**; its base is one
voxel. Throat hackles are feather texture and this asset has none. The iris is
inside a two-voxel eye. A fourth statement — "the underparts of adult male
common starlings are less spotted than those of adult females at a given time
of year" — *is* expressible, as `bird.mark_strength`, but the source gives no
number and inventing one is exactly what this document is written to avoid.

**7. rock-pigeon neck iridescence — the difference is an extent, not a
category.**
> "The adult female is almost identical in outward appearance to the male, but the iridescence on her neck is less intense and more restricted to the rear and sides, whereas that on the breast is often very obscure."
> — <https://en.wikipedia.org/wiki/Rock_dove>

The pigeon's iridescent collar measures **113 voxels of 1,187** — easily large
enough. It is rejected on a different ground: the only thing the `alt`
mechanism can say about a marking is which marking it is, and the only
available version of "less intense and more restricted" is `none`, which
contradicts the source. Expressing it properly needs a per-sex `mark_width`,
and the source publishes no number to set it to.

**8. great-tit ventral stripe — the stripe is not drawn at all.**
> "the bib is less intensely black, as is the line running down the belly, which is also narrower and sometimes broken." / "The width of the male's ventral stripe, which varies with individual, is selected for by females."
> — <https://en.wikipedia.org/wiki/Great_tit>

Two reasons and either is sufficient. The great tit's spec carries
`body_mark: none`, so there is no belly stripe on the asset for a sex to
narrow. And no source reached states the stripe's width in mm for either sex,
so even with the stripe drawn there would be nothing to set the difference to.

**9. barn-swallow plumage — an intensity the palette cannot say.**
> "The female is similar in appearance to the male, but the tail streamers are shorter, the blue of the upperparts and breast band is less glossy, and the underparts paler."
> — <https://en.wikipedia.org/wiki/Barn_swallow>

"Less glossy" is a difference of intensity in one hue. The plumage palette has
`plume_iridescent` and then `skin_dark`, with nothing between; substituting the
latter says she is a different colour rather than the same colour less bright.
The tail half of the same sentence is ADOPTED above.

**10. The eye — two voxels, no `alt` slot, on purpose.** Seven of the eight
colour slots have an `alt`; the eye does not. No species here has a documented
iris difference a two-voxel eye could carry, and the one that comes closest —
the starling, above — is rejected on its own line. A slot no species can ever
author is a slot that sits at its default forever while looking like a feature.

**11. Plumage on the remaining eleven — monomorphic, and sourced where a source
says so.** Golden eagle: *"Adults of both sexes have similar plumage and are
primarily dark brown."* Herring gull: *"Male and female plumage are identical
at all stages of development, but adult males are often larger."* Common
Scarlet macaw: monomorphic in Wiedenfeld's series, where the
real variation is subspecific — *A. m. cyanoptera* has blue-tipped yellow
coverts and no green band — and not sexual.

Grey heron: the specialist source says it outright, and it also **overturns a
field-guide claim reached earlier in this work**. A general bird site
(oiseaux-birds.com) states that "female has shorter aigrets", which would have
been a drawable difference — the heron's crest is real geometry here. The
IUCN-SSC Heron Specialist Group contradicts it:
> "Variation: The sexes are identical in plumage, but the male averages larger (Boev 1987a, b, c)."
> — <https://heronconservation.org/herons-of-the-world/list/grey-heron/>

and attributes crest length to season ("during breeding, the black crest plumes
attain full development") and to subspecies (*A. c. monicae* has "shorter
crest"), not to sex. The specialist group is the better authority, so
**`bird.crest` on the grey heron is a seasonal parameter, not a sexual one**,
and the aigret claim is recorded here as rejected rather than quietly dropped.

Common buzzard, common raven, Eurasian jay, European robin, song thrush, tawny
owl, Eurasian hoopoe: no source reached describes a plumage difference. For the
jay in particular, Wikipedia's description section contains no sex comparison at
all, which is weaker evidence than a positive statement of monomorphism and is
recorded as such rather than as a finding. The buzzard is the same and is worse:
it is the most individually variable bird in the set for reasons that have
nothing to do with sex, so "no source says the sexes differ" has more ways to be
an oversight here than elsewhere.

---

## What was NOT re-authored, and why that differs from the fish

The aquatic work found three species of twenty-three authored at one sex's
figures while claiming to be the species — an orca's dorsal at 26% of body
length is a bull's — and re-authored all three onto the average.

**No bird's numbers were changed.** The same check was run: for every species
carrying a ratio, `sqrt(r)` and `1/sqrt(r)` were applied to the authored length
and both results checked against the published range for that sex.

| species | authored | male drawn | female drawn | published |
|---|---|---|---|---|
| herring-gull | 0.60 m | 0.618 | 0.583 | male 0.60–0.67, female 0.55–0.62 |
| golden-eagle | 0.85 m | 0.820 | 0.881 | 0.66–1.02, no sexed range published |
| common-buzzard | 0.52 m | 0.507 | 0.533 | 0.40–0.58, no sexed range published |
| mallard-duck | 0.58 m | — | — | 0.50–0.65, no sexed range published |

Every one lands inside. The rule applied is the aquatic one verbatim —
re-author only where leaving the number puts *both* sexes outside their
published ranges — and nothing here does.

### What DID change on assets the owner already signed off

Nothing in geometry, and one thing in paint. Measured by rebuilding every bird
with the new rows stripped back out and diffing the voxels:

- **All twenty draw a byte-identical animal at `unsexed` with no ratio
  authored.** The twelve null species are identical in every way, including
  which individual each seed draws.
- **The eight species that now carry a ratio or a plumage declaration reseed
  once.** Their spec bytes changed, `pipeline.rng_for` hashes the spec, so
  `golden-eagle` seed 7 is a different individual eagle than it was — the same
  species at the same average, a different draw from it. This is the same event
  the pose work recorded when five species reseeded, and it is unavoidable:
  authoring anything at all into a spec reseeds it.
- **Nineteen of the twenty are byte-identical at their species average**
  (`variation.amount = 0`), which is the check that says the species itself did
  not move.
- **The twentieth is the common kestrel, and it is exactly what it should be.**
  Its silhouette is byte-identical — 2,123 voxels, occupancy identical voxel for
  voxel — and its speckle count is unchanged at 203. What moved is **which 180
  voxels the speckles landed on**, because the speckle phase is drawn from the
  seed and the seed is a hash of the spec. It is the same kestrel with its spots
  in different places. No other species in the library has a noise-placed body
  marking, which is why it is alone here.

**But four species were authored at one sex, and it is in the colour.** The
mallard's colours are a drake's, the kestrel's are a male's, the woodpecker's
crimson nape is a male's and the ptarmigan's comb is a male's. That is exactly
the trap the fish work found, arriving through a different door. It could not
be fixed the same way, because a ratio can be split down the middle and a
colour cannot. So it is **declared** rather than corrected:
`bird.sex_plumage` records which sex each was authored as, and the probe prints
the list every run.

---

## Sourcing, and one warning carried forward

This session's WebSearch budget ran out early. Everything above was gathered by
direct document fetch and by the Crossref and Europe PMC REST APIs, which is
the fallback a previous agent on this project found worked.

**Three of the four authored numbers rest on primary papers whose PDFs were
downloaded and re-extracted here**, not on a summary of them: Bortolotti 1984
for the eagle, Ingolfsson 1969 and Threlfall & Jewer 1978 for the gull, and
Wiedenfeld 1994 for the macaw rejection. Every table quoted from those is
quoted from text pulled out of the PDF in this session.

**The rest is Wikipedia, and that is a weakness, not a choice.** BTO BirdFacts —
the obvious source for British ringing biometrics, and the one that would have
given wing chords by sex for every passerine here — returns HTTP 403 to this
tool, as does AllAboutBirds. Birds of the World is paywalled. Every Wikipedia
figure above is quoted verbatim rather than paraphrased, so that a reader can
check what was actually read.

The reason for that discipline: an agent on this project once documented an
orca's eye patch as "21.8 x 5.9 cm, aspect 3.7:1, measured" in three separate
places, and no such measurement exists — the numbers turned out to match two
dimensionless indices in an unrelated paper. So the rule here is that a number
without a quotation is not shipped, a number derived from another number says
DERIVED and shows the arithmetic, and "no source found" is written down as an
answer.

Five specific gaps, named so that they can be closed:

1. **No sex-specific linear measurement for the mallard or the common raven**
   was found on any source reached; both are authored at 1.00 on size, and both
   are large enough that a real ratio would clear the floor — the mallard needs
   1.037 and the raven 1.035.
2. **The grey heron is the one open ADOPT.** A second reader on this work
   reports the Lislevand et al. avian body-size dataset as giving wing 457 mm
   for 20 males against 443 mm for 12 females, a ratio of **1.032**. The heron
   is 80 voxels and needs 1.025, so if that figure holds it clears the floor at
   about 2.5 voxels and the heron should carry `sex_length: 1.03`. **It is not
   authored, because that row was not read here** — no copy of the dataset was
   reached — and this document's own rule is that a number nobody in this
   session read does not get shipped. It is written down so that the next person
   can check one table and close it.
3. **The common buzzard's 0.95 sits below both published estimates** (0.953 and
   0.972) and the two do not agree with each other. See the ADOPT entry.
4. **The rock ptarmigan's plumage rests on two thin Wikipedia sentences.**
5. **The barn swallow's 1.20 is a geographic extreme**, not a species mean. A
   species mean would be somewhere between the Spanish 1.05 and the Finnish
   1.20, and would probably fall under the floor. The northern figure is
   authored because the alternative is not to draw the most famous tail
   ornament in ornithology at all; that is a judgement, and it is recorded as
   one.

---

## Verifying it

    python tools/birdprobe.py --sex        # the table above, measured
    python tools/birdprobe.py --sex-ab     # female / unsexed / male, rendered

`--sex` prints four things and each fails on its own: the movement in voxels
per species; the repainted area against that species' own marking-phase noise;
that `unsexed` sits between the two sexes on everything that moved; and that a
male and a female of one seed are **different individuals** — because sex
reseeds, which is the opposite of what `bird.pose` does, and is checked on all
twenty species rather than assumed.

It then runs two further tables that exist because of holes found while
building this:

- **every `alt` row on one rig.** Six of the seven colour slots are used by one
  species and two of the ten rows by none, so most of the mechanism would
  otherwise never have been executed. All ten are set on a species that authors
  none of them and measured before and after, with a control that they do not
  leak to the sex the spec is authored as.
- **the other sex's colours against the contrast floor.** `--read` reads
  `materials.bird_*` directly, so it has always graded the plumage a spec is
  authored in. The day the mallard grew a female, that gate silently stopped
  covering a fifth of the library's colour decisions — and it caught a real
  defect on its first run: the hen mallard left at the drake's yellow bill
  measured a contrast ratio of **1.20** against her buff head. An invisible
  bill, which is the exact defect ten of these twenty species shipped once
  before. Her bill is `beak_horn` at 3.39.
