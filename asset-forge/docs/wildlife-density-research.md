# Real densities, and the thing they do not fix

The owner set the wildlife spawn radius to 1–2 km, made animals killable, and
then corrected an assumption I had made out loud: *"Not every species that lists
a biome will coexist there at full density. The animal species, like their
actual earth counterparts, will be spread out and spawn at realistic density
numbers."*

He is right, the real numbers are now in `refs/density-reference.json` via
`tools/densityref.py`, and **they fix something more important than the number I
went looking for.** Written 2026-08-16.

---

## 1. The source and the tiers

[PanTHERIA](https://esapubs.org/archive/ecol/E090/184/metadata.htm) (Jones et
al. 2009, *Ecology* 90:2648), column `21-1_PopulationDensity_n/km2`: 5,416
mammal species, 956 carrying a measured density compiled from the primary
literature. The ESA archive states the data are free for scientific use and asks
only for citation.

It joins to the 131 hand-checked GBIF binomials already in
`refs/species-latin.json`. Coverage of our quadrupeds:

| tier | n | what it is |
|---|---|---|
| **measured** | 81 | PanTHERIA's own species-level figure |
| **allometric** | 26 | predicted from body mass *and* trophic level |
| **genus** | 10 | median of congeners that have one |
| **none** | 14 | no mammal source. Not estimated. Listed. |

Thirteen of the 14 are reptiles and amphibians — PanTHERIA is a mammal database
and does not cover them at all. The fourteenth is `arctic-ground-squirrel`, which
PanTHERIA carries but with no density recorded. **Every binomial in the library
is now hand-checked**, which was not true when this file was first written; §7 is
what changed.

**117 species now carry a number, spanning 5.5 orders of magnitude**, from
`wolverine` at 0.0079/km² to `rock-hyrax` at 2,735/km². Median 2.9/km².
(First written as 113 species and 5,752/km²; §7 explains what changed.)

## 2. The allometry is fitted here, not quoted

Damuth's law says density falls with body mass to about the −0.75 power. Fitted
over the 947 PanTHERIA species carrying both figures, the slope comes out at
**−0.741** — the textbook value, reproduced on this data, which is the reason to
trust the fallback at all.

Mass alone is not enough and the residuals said so before I looked for a reason.
Fitted against body length, the worst outliers were `wolverine`, `african-wild-dog`,
`grey-wolf`, `cheetah` and `eurasian-lynx`, all far *below* the line, and
`rock-hyrax` 592× *above* it. Every one of the low ones is a predator. Splitting
the fit by PanTHERIA's own trophic level, for a 10 kg animal:

| | density at 10 kg | fit |
|---|---|---|
| herbivore | **16.91** /km² | log₁₀D = 4.435 − 0.802·log₁₀(mass g) |
| omnivore | **5.20** /km² | log₁₀D = 3.831 − 0.779·log₁₀(mass g) |
| carnivore | **0.89** /km² | log₁₀D = 3.833 − 0.970·log₁₀(mass g) |

A nineteen-fold spread at identical body size. That is the energy pyramid, and a
model without it puts as many wolves on the hill as deer. Residual scatter is
~0.74 decades, so an allometric estimate is good to about a **factor of seven** —
recorded per species, because that is fine for choosing between "one per km²" and
"a hundred" and useless for anything finer.

## 3. What the authored rows were, measured against life

`herd.per_hectare` against the real figure, worst first:

| species | authored /ha | real /ha | authored is |
|---|---|---|---|
| african-wild-dog | 0.10 | 0.0001 | **1,000× too many** |
| grey-wolf | 0.05 | 0.0001 | 500× |
| cheetah | 0.03 | 0.0001 | 300× |
| plains-zebra | 0.80 | 0.0029 | 272× |
| wolverine | 0.02 | 0.0001 | 253× |
| … | | | |
| european-mole | 0.50 | 9.26 | 0.1× — **18× too few** |
| rock-hyrax | 4.00 | 27.35 | 0.1× |
| black-tailed-prairie-dog | 6.00 | 18.30 | 0.3× |

**Wrong in both directions at once.** Predators overstated by two to three orders
of magnitude, small mammals understated by ten. That is exactly what you get from
numbers authored by eye for near-field appearance: everything drifts toward "a
few of these visible at once."

This is the real prize. A grey wolf at 0.01/km² is **one wolf per 100 km²** — an
event, not scenery. No global multiplier could have produced that from the
authored table, because the authored table had wolves at merely a fifth of zebra
density instead of a two-hundredth.

## 4. And now the part I got wrong

I expected real densities to solve the population problem. **They make the total
worse.** Quadrupeds within a 2 km radius (1,257 hectares):

| biome | authored | from real densities |
|---|---|---|
| grassland | 23,469 | **46,480** |
| savanna | 11,465 | **30,225** |
| desert | 10,409 | **32,826** |
| taiga | 9,854 | 6,789 |

Because the total is dominated by the animals nobody came to see. A European
mole at 926/km² is 11,600 moles inside the bubble; a rock hyrax at 2,735/km² is
34,000. Both figures are *correct*. Neither is a reason to instantiate anything.

Checking whether they are at least asleep does not rescue it either — of the
density leaders, `rock-hyrax`, `black-tailed-prairie-dog`, `pika` and
`eastern-grey-squirrel` are all diurnal. (PanTHERIA's activity cycle is carried
into the reference anyway, since a spawner should not have to guess.)

## 5. So the conclusion is a design one, not a data one

**Density decides composition. Body size decides existence.**

Real densities are exactly right for deciding *what is rare* — and that is the
half that makes a world feel alive, because rarity is what makes an encounter
mean something. They cannot decide *how many entities exist*, because a
correct density for a mole is an argument for 11,600 of something 15 cm long
that is invisible past 30 m and spends its life underground.

The entity range has to scale with body size, which the authored `despawn_m`
already half-knows — the elephant is at 800 m and the median is 220 m. What is
needed on top of the density baseline:

* a **size floor for the population ring**: below some body length a species
  exists only in the near ring, because it cannot be seen further out anyway. A
  1.5 m deer is 1.3 px at 1 km; a 30 cm hyrax is sub-pixel at 200 m.
* **group records, not individuals**, beyond the actor ring — a herd of eight is
  one thing with a count.

Neither is a density question, and neither is blocked on the missing species
below.

## 6. What is not sourced yet, and what it needs

**Birds (127), fish (106) and cetaceans (18) have no densities**, for two
separate reasons:

1. PanTHERIA is mammals only. Cetaceans are mammals and would be covered — but
2. only the 131 quadrupeds have scientific names on file at all.

The names cannot be bulk-resolved. `tools/refnames.py` records what happened
when that was tried: GBIF returned *Macaca silenus* (a lion-tailed macaque) for
`lion`, *Mungos mungo* (a banded mongoose) for `nile-monitor`, and
*Peratherium cuvieri* (an extinct marsupial) for `fisher` — each a single
confident result with a clean binomial, which then produced a valid, internally
agreeing, completely wrong reference. Names get hand-supplied and GBIF-verified,
or they do not get used.

Birds and fish then need different sources entirely: breeding-bird survey
densities in territories/km² for birds, and standing biomass per hectare for
fish, which is not the same quantity as a count and will need converting through
body mass.

**The 18 unsourced quadrupeds** are listed by `densityref.py report`. Thirteen
are reptiles and amphibians needing a herpetological source; five are mammals
that would resolve if their binomial were hand-checked — including `capybara`
and `european-hedgehog`, which are on file with misspelt genera (`Hydrochoeris`,
`Erimaceus`).

## 7. The watch item, chased down and closed

This section originally flagged a risk: the two highest densities in the library
both came from binomials marked `checked_by_hand: false`, and `rock-hyrax` was on
file as *Heterohyrax brucei* — the **yellow-spotted** hyrax, not *Procavia
capensis*. It sat at the top of the table where it moved every total most.

**Chased down 2026-08-16, and it was wrong.** Corrected to *Procavia capensis*
and re-verified through `tools/refnames.py`'s check (ACCEPTED, SPECIES rank,
Mammalia, GBIF's own vernaculars containing "rock" and "hyrax"), the density is
**2,734.5/km², not 5,752** — the wrong hyrax was inflating the library's largest
number by 2.1×.

Three more names were wrong the same way and nobody had ever checked them:

| species | was | is |
|---|---|---|
| `capybara` | *Hydrochoeris hydrochaeris* | *Hydrochoerus hydrochaeris* |
| `european-hedgehog` | *Erimaceus roumanicus* | *Erinaceus europaeus* |
| `chamois` | *Rupicapra carpatica* | *Rupicapra rupicapra* |

All three had failed to join PanTHERIA at all, so they were sitting in the
"no source" pile; corrected, they carry **125.3, 123.0 and (still none)**
respectively. Coverage went from 113 species to **117**, unsourced from 18 to 14.

**Two more needed a different fix, and it is worth distinguishing.** `fisher` and
`arctic-ground-squirrel` had names that were *right and current* — but PanTHERIA
is keyed on 2005 taxonomy, and both have been reassigned since (*Pekania* was
*Martes*, *Urocitellus* was *Spermophilus*). GBIF returns the old names as
SPECIES/**SYNONYM** with vernaculars still matching, which is exactly what a
superseded-but-correct name looks like. Those go in a `WR05_SYNONYM` bridge in
`densityref.py` — the accepted name identifies the animal, the old name finds its
row — and never into the name file, which holds identity.

**And the remaining fourteen unverified names were all correct.** Each was put
through the same check and all fourteen passed, so they are now in the hand table
too. The warning is therefore *earned*: an unverified name now means nobody
looked, and after this pass nobody's name is unlooked-at.

The lesson is the one this project keeps relearning. The wrong hyrax produced a
perfectly valid density, for a real animal, from a real study, joined correctly
to a real row. Nothing was malformed. It was simply a different animal — and it
was the biggest number in the table.
