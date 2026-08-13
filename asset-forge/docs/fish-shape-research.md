# Generating fish shapes: what the literature offers, and what survives 25 voxels

Research behind `forge/fish.py`. Done before the generator was written, and the
generator is what it is because of it.

**The question this had to answer** was not "how do you model a fish". It was
"how do you get from a species name to a fish that is twenty to thirty voxels
long and still recognisable". Those are very different questions, and almost
everything published answers the first one.

**The finding that shaped everything else**: at 25 voxels of length, a fish's
cross-section is between 1 and 5 voxels wide. Most of the sophisticated shape
literature — superformula curves, elliptic Fourier descriptors, reaction–
diffusion pattern chemistry — describes structure an order of magnitude finer
than that. It is not that those methods are wrong; it is that at this size their
output rounds to the same voxels as a much simpler method's. Section 8 says
what was adopted and what was rejected, with the numbers.

---

## 1. Body shape: the vocabulary, and the numbers behind it

Ichthyology already has the parameter this generator needed. Bodies are
classified into a small set of named forms:

| Class | What it means | Examples |
|---|---|---|
| Fusiform | streamlined torpedo | tuna, mackerel, bass, trout |
| Compressiform | flattened side to side | angelfish, bream, most reef fish |
| Depressiform | flattened top to bottom | rays, flatfish, bullheads |
| Anguilliform | eel-like | eels, lampreys, loaches |
| Sagittiform | arrow-shaped, fins pushed aft | pike, gar, barracuda |
| Globiform | spherical | pufferfish, lumpfish |
| Taeniform / filiform | ribbon / thread | oarfish, snipe eels |

Sources: University of Hawai'i,
[*Exploring Our Fluid Earth*](https://manoa.hawaii.edu/exploringourfluidearth/biological/fish/structure-and-function-fish);
[CIMI, *Get in Shape with Fish Forms and Functions*](https://cimi.org/blog/get-in-shape-with-fish-forms-and-functions/).

**These classes are not separate shapes. They are one shape at different
ratios**, which is why `fish.depth_ratio` and `fish.width_ratio` are sliders
here rather than a menu.

### The ratios, measured rather than quoted

Published per-class ratio tables essentially do not exist. The numbers below
were computed from **FishShapes v1** (16,523 specimens, about a fifth of all
teleost species — [Zenodo mirror](https://zenodo.org/records/6642650)) joined
by name to FishBase's own shape classification, a 93% join.

| FishBase class | n | length : depth | depth : width |
|---|---|---|---|
| eel-like | 596 | **16.8** (p10 10.3, p90 34.9) | 1.33 |
| elongated | 3,028 | **5.5** (3.7–9.0) | 1.37 |
| fusiform / normal | 4,620 | **3.2** (2.4–4.7) | 1.91 |
| short and/or deep | 2,108 | **2.0** (1.5–3.0) | **2.78** |

Independently confirmed by FISHMORPH, 8,342 species
([figshare](https://doi.org/10.6084/m9.figshare.14891412)): fusiform 3.54,
elongated 4.95, short/deep 2.39, eel-like 13.1. And by Infinigen's
hand-authored templates (§7): bluefish 3.5, crappie 2.4, pickerel 6.1, eel 11.0.
Three independent sources agreeing to within about 10% is as solid as this
gets.

All-teleost medians, n = 16,430: length:depth **3.77** · depth ÷ standard length
**0.27** · depth:width **1.72** · head depth ÷ body depth **0.62** · caudal
peduncle depth ÷ body depth **0.34**.

### The same table in voxels, for a 25-voxel fish

This is the version that decided the design:

| Class | depth | width | head depth | peduncle depth |
|---|---|---|---|---|
| eel-like | 1.5 | 1.0 | 0.8 | 0.2 |
| elongated | 4.6 | 3.4 | 3.0 | 1.8 |
| fusiform | 7.8 | 4.1 | 4.8 | 2.6 |
| short/deep | 12.3 | 4.5 | 7.5 | 3.0 |

**Depth spans 1.5 to 12.3 voxels — a factor of eight. Width spans 1.0 to 4.5,
and never exceeds five voxels for any class.** So depth is the axis that
separates one fish from another and width is nearly a constant. That is why
`fish.depth_ratio` is documented as the biggest lever in the group and
`fish.width_ratio` is not.

Note the last column. **An eel's caudal peduncle is 0.2 voxels.** It does not
exist at this resolution, and any code that assumed a body always has a
cross-section containing at least one cell centre would produce an eel in three
pieces. That is exactly what happened, and it is why the body axis is stamped
as a solid one-voxel run before any cross-section is drawn.

### Ecomorphological indices (Gatz 1979 and successors)

Compression index = depth ÷ width. Relative depth = depth ÷ standard length.
Ventral flattening = midline-to-belly ÷ max depth (low means fast water).
Relative head height = head depth ÷ body depth. Vertical eye position (high
means bottom-living). Definitions from
[SciELO, Amazonian floodplain assemblages](http://www.scielo.br/j/bjb/a/PCG3hVG7ZNdwRLFNMCvYcVv/?lang=en).

**Fineness ratio** — length ÷ equivalent diameter — has a drag optimum at
**4.5**, and fast reef fish cluster there
([PLOS ONE](https://pmc.ncbi.nlm.nih.gov/articles/PMC3799785/)). Honest caveat
from that same paper: its correlation with actual swimming speed is weak, and
for body-and-tail swimmers it is *negative*. Treat 4.5 as an aesthetic anchor,
not a law.

---

## 2. Fins

Dorsal and anal fins give stability; the caudal gives propulsion; pectorals
steer and hover; pelvics brake and control pitch; the adipose fin is a small
fleshy lobe behind the dorsal found on salmonids, catfish and characins and
almost nothing else.

### Caudal shapes, with FishBase frequencies and measured aspect ratios

| Shape | FishBase species | Aspect ratio (median) | What it is for |
|---|---|---|---|
| forked | 4,820 | 1.88 (p90 3.08) | speed and manoeuvre together |
| truncate | 4,263 | 1.36 | acceleration and turning; reef and bottom fish |
| pointed | 361 | 0.65 | slow and precise |
| heterocercal | 107 | 1.70 | sharks and sturgeon |

Lunate is not a FishBase category — it is the top of the forked range. Tuna
measure about 7, scombrids 4–9, with a maximum observed 7.48. **This is why
`fish.caudal_shape` has five entries and lunate and emarginate are not among
them**: they are a deep and a shallow fork, so they come from `caudal_fork`, and
a choice that duplicated a slider position would be a choice that silently
ignored it.

Counter-result worth knowing: a
[*Journal of Experimental Biology* study](https://journals.biologists.com/jeb/article/225/22/jeb244967/284368/)
found truncate tails can out-perform forked ones when cruising. The
shape–speed story is folklore-tidier than the data.

### Fin positions, as a fraction of standard length

Computed from **7,452 landmarked FishBase specimens**:

| Landmark | p10 | median | p90 |
|---|---|---|---|
| Eye, front edge | 0.040 | **0.072** | 0.132 |
| Operculum (end of head) | 0.209 | **0.278** | 0.362 |
| Pectoral origin | 0.214 | **0.285** | 0.365 |
| Pelvic origin | 0.236 | **0.362** | 0.541 |
| Dorsal origin | 0.235 | **0.360** | 0.585 |
| Deepest point of body | 0.326 | **0.413** | 0.488 |
| Anal origin | 0.454 | **0.638** | 0.774 |
| Caudal fin height ÷ SL | 0.145 | 0.259 | 0.369 |

**Three of these are hard-coded in `forge/fish.py` rather than exposed as
sliders** — the eye at 0.072, the pelvic origin at 0.36, and the anal fin
ending just before the wrist. Each has one right answer, and a slider whose
only correct setting is one value is a slider that can be set wrong.

Infinigen's hand-tuned placements invert to almost exactly these numbers, which
is a useful independent check on both.

Swimming modes, for completeness
([Sfakiotakis et al., free PDF](https://www.societyofrobots.com/robottheory/Review_of_Fish_Swimming_Modes.pdf)):
anguilliform → subcarangiform → carangiform → thunniform is a sequence of
decreasing body involvement and increasing speed. FishBase records it for 2,635
species. Nothing in this generator uses it, because nothing here moves yet.

---

## 3. Superformula and superellipse cross-sections

The 2D superformula (Gielis), exactly:

```
r(φ) = ( |cos(mφ/4)/a|^n₂ + |sin(mφ/4)/b|^n₃ ) ^ (−1/n₁)
```

`m` sets rotational symmetry, `n₁` bulges the sides in or out, `n₂` and `n₃`
round alternating nodes, `a` and `b` scale the axes. `m=4, n₁=n₂=n₃=2` is an
ellipse. Sources: [Wikipedia](https://en.wikipedia.org/wiki/Superformula),
[Paul Bourke](https://paulbourke.net/geometry/supershape/).

The plain superellipse, parametrically
([BIT-101](https://www.bit-101.com/2017/2023/01/coding-curves-13-superellipses-and-superformulas/)):

```
x = |cos t|^(2/n) · rx · sign(cos t)
y = |sin t|^(2/n) · ry · sign(sin t)
```

`n=1` diamond · `n=1.5` pointed · `n=2` ellipse · `n=4` squircle · `n=10+`
rectangle. A knife-edged laterally compressed section is `n < 2` **plus**
`ry ≫ rx` — the exponent alone does not do it.

**Published use of the superformula for fish cross-sections: none found.**
Gielis fitted bamboo, diatoms, flowers and shells. FishBase does classify
cross-section categorically (`oval`, `compressed`, `circular`, `flattened`,
`angular`), which is more useful here anyway.

### The test that settled it

Superellipses were rasterised at the cross-section sizes this generator
actually produces, and the differing voxels counted against `n = 2`:

| Cross-section | n=1 | n=1.5 | n=3 | n=4 | n=8 |
|---|---|---|---|---|---|
| **4 × 8** (fusiform, 25 vox) | 12 | 4 | **0** | 4 | 4 |
| 5 × 9 (fusiform, 30 vox) | 16 | 8 | 4 | 4 | 8 |
| 3 × 5 (elongated) | 4 | **0** | 4 | 4 | 4 |
| 5 × 12 (deep-bodied) | 16 | 4 | 8 | 8 | 12 |

At 4 × 8 — the commonest case here — **n = 2 and n = 3 are pixel-identical**,
and every other exponent differs by exactly four voxels: the corners.

`fish.section` survives as one slider anyway, because on a deep-bodied fish
(5 × 12) the difference reaches 16 voxels and a knife-edged back is visible. But
it is one exponent, not six, and `tools/fishprobe.py` measures what it buys
(section fill moves from 26.5% to 35.1% of the bounding box across its range).
The full superformula would start earning its keep at roughly twice this voxel
budget.

---

## 4. Elliptic Fourier descriptors, landmarks, and the dataset question

Real work exists.
[Caillon et al. 2018, *Ecosphere*](https://doi.org/10.1002/ecs2.2220) took 218
North Sea fish outlines and needed 14 harmonics for 99% cumulative power;
[a tutorial with code](https://rfrelat.github.io/FishMorpho.html) reproduces it.
A Senegalese sole study used 20 harmonics on 2,271 fish.
[Loy et al. 2000](https://www.sciencedirect.com/science/article/abs/pii/S0144860999000357)
compares landmark geometric morphometrics against outline fitting head to head.

Landmark schemes vary but converge on an 11–16 point core: snout tip, gape
corner, eye, occiput, operculum, pectoral insertion, dorsal origin and
insertion, pelvic origin, anal origin and insertion, caudal peduncle upper and
lower, hypural midpoint. Procrustes superimposition = centre, scale to unit
centroid size, rotate to minimise squared distance.

Sobering result from
[Moccetti et al. 2023, *PeerJ*](https://doi.org/10.7717/peerj.15545): four
operators digitising the *same photographs* were identifiable from the shape
data alone with 83% accuracy. Landmark data carries the digitiser.

### Is there a public dataset of fish outline harmonics?

**No. Nowhere.** Every paper computes its own and publishes the analysis rather
than the coefficients. What does exist, all verified as downloadable:

1. **FishBase as parquet** — [huggingface.co/datasets/cboettig/fishbase](https://huggingface.co/datasets/cboettig/fishbase).
   `species.parquet` carries a body-shape class for **36,125 of 36,132
   species**. `morphdat.parquet` (22,243 rows) carries cross-section class,
   caudal shape, horizontal/vertical/diagonal stripe flags, spot flags, mouth
   position and adipose-fin presence. `morphmet.parquet` holds **13,293
   specimens of 2D landmark coordinates** — a public fish landmark dataset that
   appears to be almost entirely overlooked.
2. **FISHMORPH** — 8,342 species, ten traits, nine of them dimensionless ratios.
3. **FishShapes v1** — 16,523 specimens; **the only source with body width**.
4. **174 per-species silhouette PNGs** — [github.com/simonjbrandl/fishape](https://github.com/simonjbrandl/fishape).

**Two licence flags, and they are the reason none of this is embedded in the
repo.** FishBase is explicitly **not licensed for commercial use**. FishShapes
carries a genuine contradiction: Zenodo and Dryad metadata both say CC0, the
dataset's own abstract says CC BY-NC. A game is a commercial product, and
neither of those belongs inside one without a conversation with the rights
holders. The species table in `forge/language.py` is twenty-odd fish typed out
by hand from the published *medians* — facts, not a database — and carries no
licence at all.

### Is EFD worth anything at 25 voxels?

**No,** and the arithmetic is worth writing down because it is the clearest
example of the general problem.

A 25-voxel outline has about 90 boundary samples, so the nominal ceiling is 45
harmonics. That is fiction. Coefficients decay as roughly A₁/n²; voxel
quantisation puts a 0.289-voxel RMS noise floor on the boundary, which works
out to a per-coefficient noise of about 0.043 voxels. Signal-to-noise reaches 5
at **n ≈ 7.6**, and truncation error drops under half a voxel at **N ≈ 6**. So
six to ten harmonics is the whole usable band.

The killer is what that band contains. A dorsal fin spans roughly 5 of those 90
boundary samples, so resolving it as a distinct lobe needs harmonic ~18 — whose
amplitude, 0.039 voxels, is *below the noise floor*. **The harmonics that would
encode the fins are exactly the ones the lattice destroys.** And ten harmonics
× four coefficients is 40 floats to describe a shape whose raw silhouette is
225 bits: negative compression.

### But silhouettes do carry identity — just not as harmonics

Worth separating, because it is the more important question. All 174 `fishape`
silhouettes were scaled to 25 voxels long and measured:

- Depth at that length: **minimum 3, median 10, maximum 38 voxels**
- 37% are 12 or more deep — deep discs, where the silhouette alone is strong
- 54% land between 7 and 11
- Only **2%** collapse to 3 or under (garfish, sea lamprey, pipefish — for
  those, identity is aspect ratio and nothing else)
- Pairwise silhouette overlap after normalising aspect away: median 0.649, and
  only 0.5% of the 15,051 pairs exceed 0.85

**So: outline carries real species information at 25 voxels. Encode it as
explicit geometry — a spine with per-station depth and width, plus fins placed
at measured positions — not as a frequency basis.** That is what `forge/fish.py`
does.

---

## 5. Colour patterns

### Turing / reaction–diffusion

[Kondo & Asai 1995, *Nature* 376:765](https://www.nature.com/articles/376765a0)
is the famous one: *Pomacanthus* angelfish stripes keep a **constant width while
the fish grows**, so new stripes are inserted between the old ones — 3 stripes
at two months, 12 at twelve — and branch points slide sideways like a zip
opening. The property that matters for a generator is that **wavelength is a
property of the chemistry, not of the body**: the same parameters on a
20-voxel and a 30-voxel fish give the same stripe width and more stripes.

Zebrafish is the mechanistically complete case
([Nakamasu et al. 2009, *PNAS*](https://www.pnas.org/doi/10.1073/pnas.0808622106)):
melanophores, xanthophores and iridophores with short-range mutual inhibition
and long-range inhibition, assembled from cell contacts rather than a diffusing
morphogen. Full parameters are published, and two mutants are one-line edits —
*leopard* spots come from changing one constant from 0.37 to **0.385**, a 4%
shift that flips stripes into spots. A real zebrafish stripe is only 7–12
pigment cells wide.

The most usable formulation is Kondo's later kernel model
([free PDF](https://www.fbs.osaka-u.ac.jp/labs/skondo/simulators/KernelPatternGeneraterGauss_Web/KTmodelPaper.pdf)):
convolve with a Mexican-hat kernel and clamp. **The 2D integral of the kernel
decides the pattern type regardless of the kernel's shape** — near zero gives
stripes and labyrinths, negative gives dark spots, positive gives light spots —
and **the wavelength is the peak of the kernel's Fourier transform**, so a
period can be dialled in directly.

Gray–Scott is the better-known route
([Pearson 1993, *Science*](https://www.science.org/doi/10.1126/science.261.5118.189);
[full class map](http://www.mrob.com/pub/comp/xmorphia/pearson-classes.html)):
F=0.030 k=0.055 hexagonal spots, F=0.050 k=0.063 branching maze, F=0.046
k=0.065 parallel stripes. **It gives excellent texture and almost no control** —
no orientation preference, wavelength not independently settable, and a change
of 0.001 in k can blank the field.

### Orientation: stripes versus bars

Turing systems have **no intrinsic orientation**; it has to be imposed.
[Shoji et al. 2003, *Developmental Dynamics*](https://www.fbs.osaka-u.ac.jp/labs/skondo/paper/shoji%20DevDyn%202003.pdf)
compared *Genicanthus melanospilos* (vertical bars) with *G. watanabei*
(horizontal stripes) — near-identical fish, opposite orientation, identical
developmental sequence. **Stripes run parallel to the activator's
fast-diffusion axis. Whichever anisotropy is larger wins; equal anisotropy
gives a labyrinth.** The transition is knife-edge — 7% anisotropy is enough. The
proposed cause is scales, and **scale-less fish (puffers, morays, catfish) get
non-directional mazes** while scaled fish get directional stripes.

### Countershading

Dark above, pale below. Two functions: cancelling self-shadow, and matching the
background from above and below at once — the second dominates in water.
[Cuthill et al. 2016, *PNAS*](https://pmc.ncbi.nlm.nih.gov/articles/PMC5135326/)
found the optimal gradient depends on the light: direct sun favours a sharp
transition, diffuse or overcast light favours a smooth ramp, and under diffuse
light a sharp transition confers no advantage at all. Mechanistically it is an
Asip1 gradient acting on the same cells the Turing system arranges, which is a
good argument for implementing it as **a separate layer underneath the
pattern** — which is both correct and free at any resolution.

### What real fish wear, and when

[Miyazawa 2020, *Science Advances*](https://pmc.ncbi.nlm.nih.gov/articles/PMC7710386/)
annotated **18,114 species into 11 pattern categories**. His mechanistic result
is elegant: labyrinth is the blend of dark-spotted and light-spotted, the same
axis as the kernel integral.

| Emit | When | Source |
|---|---|---|
| Vertical bars | deep body; reef, weed or rock; ambush; solitary | Seehausen 1999; [Urban 2022](https://pmc.ncbi.nlm.nih.gov/articles/PMC8820146/) |
| Horizontal stripes | elongate body; schooling; fast or pelagic | Urban 2022: correlated with elongation, **p = 0.009** across 461 cichlids; evolved ~73 times independently |
| Maze | scale-less, smooth-skinned | Shoji 2003 |
| Countershading | always, except benthic flatfish | Cuthill 2016 |
| Band through the eye | always | Barlow 1972 |
| Caudal eyespot | small or juvenile only | Hemingson 2020 |
| **Never** | bars and stripes together | Seehausen 1999 — mutually exclusive |

[Barlow 1972, *Copeia*](https://www.jstor.org/stable/1442777) on the eye band:
its angle correlates with body shape. Fast elongate fish have horizontal eye
lines, deep-bodied barred fish have vertical ones — **the eye line is the body
pattern continued across the head, not an independent mark**. Gavish & Gavish
1981 add that concealment works best when the eye sits *on the boundary* of the
dark patch rather than centred in it.

FishBase already records the presence of each of these per species:
horizontal stripes 1,125 present / 1,900 absent, vertical stripes 759 / 2,123,
spots 1,421 multi / 381 single / 1,490 none.

---

## 6. Readability at very low resolution

**This generator has more resolution than Minecraft does**, which is worth
knowing before deciding anything is impossible. Mojang's shipped geometry, from
[github.com/Mojang/bedrock-samples](https://github.com/Mojang/bedrock-samples):

| Model | Length | Height | Thickness |
|---|---|---|---|
| tropical fish A | 10 px | 7 px | **2 px** |
| tropical fish B | 11 px | 16 px | **2 px** |
| cod | 17 px | 6 px | 2 px |
| salmon | **25 px** | 7 px | 3 px |

Every one is 2–3 units thin, and all the species signal lives on one flat flank
of 18–36 texels. Their fins are **zero-thickness planes with transparency** — a
cheat unavailable in cubic voxels, which is why fins here cost a real voxel and
have to be budgeted.

**The single most useful number found in this whole exercise**: Minecraft's
tropical fish system can generate 3,072 variants (2 shapes × 6 patterns × 16
base colours × 16 pattern colours), and **90% of the fish a player meets are
one of 22 hand-curated presets. Only 10% are randomised.** Mojang built a large
space and then almost always showed people a small curated corner of it,
because familiarity is what recognisability is made of. Their curated set uses
only 11 of the 16 available dyes.

Measured from their pattern textures: ink coverage runs **19–67%, mean about
41%** — never near 0 or 100. The preset chosen for both clownfish variants is
literally two vertical bars. The scattered-single-pixel patterns read as noise
rather than as identity.

**Why value matters more than hue.** Minecraft's body texture is pure greyscale
tinted multiplicatively, and the pattern overlay is near-white, so the pattern
is about 25% brighter than the body *whatever colours the RNG picks*. Mojang did
not gate on colour contrast and paid for it: 16% of random dye pairs come out
below a contrast ratio of 1.2, and one preset at 67% ink and CR 1.04 genuinely
reads as a plain blue blob.

The physical reason is that the visual system carries luminance at roughly four
times the spatial bandwidth of red-against-green — the same fact that makes
chroma subsampling invisible in video. **A hue-only edge blurs away about four
times sooner than a value edge.** That is the basis under
[Slynyrd's "value first, hue later"](https://www.slynyrd.com/blog/2018/1/10/pixelblog-1-color-palettes)
and under the squint test.

**The two-pixel floor**: 20/20 acuity is about one arcminute per element and a
resolvable cycle needs two pixels, which is the real origin of the pixel-art
rule that features must be at least two across.
[Material Design](https://m1.material.io/style/icons.html) mandates 2 dp strokes
for the same reason; Apple's icon guidance is the practice version — Safari's
degree tick marks exist at 512 px and are deleted at 16.

Also: [Pixel Logic](https://archive.org/stream/pixel-logic-a-guide-to-pixel-art-michael-azzi/Pixel%20logic%20a%20guide%20to%20pixel%20art%20-%20Michael%20Azzi_djvu.txt)
("2–3 main colours improves recognizability" — and its own worked example is a
fish); [Blockbench's Minecraft style guide](https://www.blockbench.net/wiki/guides/minecraft-style-guide/)
("small objects being recognizable takes priority over being to scale");
[Sketchfab, *Reducing the Greebles*](https://sketchfab.com/blogs/community/voxel-art-reducing-greebles/)
("noise will only cloud the design you intend to make").

### What actually fits on a 24-voxel fish

| Element | Minimum that reads | How many fit |
|---|---|---|
| **Vertical bars** | 2 on, 2 off | **4–5** — the most legible pattern available |
| Horizontal stripe | 2 on, 2 off | 2–3 |
| Spots | 2 × 2, never 1 × 1 | about 10 |
| Maze / labyrinth | period 3–4 | **do not** — it becomes salt and pepper |
| Diagonals | period 5+ | barely; stair-steps into vertical |
| Countershading | 2 tone steps | free, and the most reliable feature available |

This table is why `river-perch` was re-authored from seven bars to five, and
why `clown-anemonefish` had to be authored at 18 cm rather than life size: a
10 cm clownfish at the 1 cm lattice is ten voxels long, and ten voxels cannot
hold three bars two voxels wide.

---

## 7. Existing procedural fish generators

**[Infinigen](https://github.com/princeton-vl/infinigen)** (BSD-3, CVPR 2023) is
the closest prior art and the one worth copying from. Its fish bodies are
`(9, 8, 3)` arrays — **9 cross-sections along the spine × 8 points around each ×
XYZ, 216 numbers per species**:

| Template | length:depth | depth:width | class |
|---|---|---|---|
| eel | 11.0 | 1.64 | anguilliform |
| pickerel | 6.1 | 1.49 | sagittiform |
| bluefish | 3.5 | 2.11 | fusiform |
| crappie | 2.4 | 1.69 | compressiform |
| spadefish | 1.3 | **3.32** | extreme compressiform |
| pufferfish | 1.6 | **0.89** (wider than deep) | globiform |

Species sampling is a convex blend of weights over named templates with a
per-species *temperature* limiting drift — and the temperatures pin eel to 0.01
and puffer to 0.001, **because blending those with anything gives garbage**.
That is a hard-won lesson worth inheriting even though this generator does not
blend.

Other things found:

- **[fishdraw](https://github.com/LingDong-/fishdraw)** (MIT) — 2D, but its
  parameter vocabulary is excellent, including fractional fin start and end.
- **[pixel-sprite-generator](https://github.com/zfedoran/pixel-sprite-generator)** —
  an under-rated idea. Its mask alphabet (−1 always border, 0 always empty, 1
  randomly empty-or-body, 2 randomly border-or-body) plus mirroring is exactly
  how to randomise a small sprite *without ever breaking the silhouette*.
- **No Man's Sky's discipline** — hundreds of fixed archetypes with identical
  rigs; variants scale parts and swap attachments and never invent topology.
- **[Procedural Fish Modeling, IEEE CG&A 2024](https://ieeexplore.ieee.org/document/10629048)** —
  the only published name-to-shape pipeline: species name → Stable Diffusion
  lateral image → differentiable-rendering curve fit. Paywalled, wildly overkill
  here, but the architecture is a fallback if 200 species are ever needed.
- **[Rune Johansen's creatures](https://blog.runevision.com/2025/01/procedural-creature-progress-2021-2024.html)** —
  "skin distance from the bone in several directions per segment", which *is* a
  voxel fish. He explicitly rejected PCA for parameterisation because the axes
  are not meaningful to author against. Heed that before fitting a latent space
  to FishShapes.

**Nobody has built a FishBase-driven generator.** That intersection is empty.

---

## 8. What was adopted, and what was rejected

### Adopted

**Lofting along a spine with a per-station depth and width profile.** Infinigen,
Johansen and this generator arrived at the same factorisation independently, and
the measured ratio tables in §1 are stated in exactly these terms. Rasterised
**directly** into voxels — project each cell onto the spine and test it against
the interpolated cross-section. No mesh, no isosurface, no voxelisation pass.

**The morphometric class as the parameter axis.** `depth_ratio`,
`width_ratio`, `depth_at`, `peduncle` and `fullness` are the numbers §1
measures, so authoring a species is reading a table rather than dragging until
it looks right.

**One superellipse exponent for the cross-section**, not the full superformula.
It is the only part of that family that still moves a whole voxel at this size
(§3), and only on deep-bodied fish.

**Fins as placed primitives at measured landmark positions** (§2), with the
three positions that have one right answer hard-coded rather than exposed.

**Caudal shape as five outlines plus a notch depth**, with lunate and
emarginate expressed as notch settings rather than as separate entries.

**Countershading as a layer underneath the marking** (§5). It is the cheapest
and most reliable feature available: two tone steps, free at any resolution, and
the only thing that gives a voxel fish a top and a bottom.

**Exactly one marking per species** (§5, §6). Bars and stripes are mutually
exclusive in nature and unreadable together at this size.

**The 2-on-2-off floor and the 4–5-bar ceiling** (§6), enforced by
`tools/fishprobe.py --read`.

**Value contrast as a hard gate** (§6). The probe computes the WCAG contrast
ratio between each species' flank and its marking and flags anything under 1.5.
This is the one recommendation that most directly prevents shipping a pattern
that exists in the voxels and is invisible in the water.

**Hand-typed species medians rather than an embedded dataset** (§4), for the
licence reasons above.

### Rejected, and why

**Reaction–diffusion / Turing pattern generation.** The mathematics is
beautiful, the fish biology is real, and the output at this resolution is
noise. A Turing pattern has a *wavelength*, and a real fish's is a few
millimetres; on a body twelve voxels deep the simulation resolves either one
stripe or salt-and-pepper, and which one you get depends on the grid rather
than on the fish. §6's own table says a maze at period 3–4 does not read. What
would be needed is running it at 4–8× supersampling, hard-thresholding,
downsampling by majority vote, morphologically opening and closing with a 2×2
element and deleting components under three voxels — at which point the output
is indistinguishable from parametric bars and there are five more places for a
silent no-op to hide. `fish.pattern` is a choice of five parametric marks
instead, and each one is measured.

**Elliptic Fourier descriptors as the shape representation** (§4). Six to ten
usable harmonics, the fin-encoding harmonics below the quantisation noise
floor, and negative compression against a raw silhouette.

**The full superformula** (§3). Pixel-identical to an ellipse at the commonest
cross-section size in this library.

**Gray–Scott specifically**, even as a texture source. No orientation control,
no independent wavelength, and a 0.001 change in one constant can blank the
field. If reaction–diffusion is ever revisited, it should be Kondo's kernel
model, where the pattern class and the wavelength are two separate legible
knobs.

**Embedding FishBase, FISHMORPH or FishShapes.** Licence, and only licence —
the data is excellent. FishBase is explicitly non-commercial and FishShapes
contradicts itself about its own terms.

**Template blending between shape classes.** Infinigen supports it and then
pins the temperature to 0.01 for eels and 0.001 for puffers to stop it
happening, which is a strong hint. A blended eel-and-pufferfish is not an
intermediate animal; it is a mistake.

**Meshing and then voxelising.** Conservative voxelisation over-fills by one to
two voxels everywhere and destroys thin fins. This is the same conclusion the
tree generator reached for the same reason — see the main research doc,
section 4.

**Fitting a latent space (PCA) to the shape data.** Johansen's objection is
decisive for a tool whose whole premise is that a designer drags sliders: the
axes are not meaningful to author against.

**A separate colour system for fish.** ADR-0008 gives every voxel face one flat
colour from the engine's material palette. Fish need colours that palette does
not have, and the answer to that is ten more materials in the palette, not a
second path. See the colour proposal in the handover.

---

## 9. The lattice

Not from the literature — measured here, with `tools/fishprobe.py --lattice`.
The terrain is 10 cm, every other asset in this library is 5 cm. A fish is
authored at **1 cm**, and the reason is not cost. Measured on seed 1, nose to
the end of the tail fin:

| Species | Lattice | Length | Depth | Fork depth | Eye | Voxels |
|---|---|---|---|---|---|---|
| brown-trout | 1 cm | 41 | 13 | **5** | yes | 602 |
| | 2 cm | 21 | 8 | 2 | yes | 113 |
| | 5 cm | 8 | 5 | **0** | no | 26 |
| clown-anemonefish | 1 cm | 18 | 13 | — | **2 vox** | 205 |
| | 2 cm | 9 | 8 | — | **0** | 46 |
| | 5 cm | 6 | 4 | — | 0 | 18 |
| northern-pike | 1 cm | 100 | 28 | **12** | 2 vox | 4,633 |
| | 2 cm | 50 | 15 | 6 | 2 vox | 736 |
| | 5 cm | 20 | 7 | 1 | **0** | 100 |

Read the two columns on the right. **At 5 cm the tail fork is gone on every
species and so is the eye.** At 2 cm both survive on a large fish and the eye is
gone on a small one. Only 1 cm carries both across the whole set — and the eye
and the tail are, between them, most of what makes a twenty-voxel object read
as an animal rather than as a lozenge.

Cost is not the constraint at any of these sizes. A whole fish at 1 cm is
150–4,600 voxels against 75,000 for a temperate oak; a shoal of forty small
fish is under a tenth of one tree. 1 cm nests **10:1** in the terrain lattice
and **5:1** in the 5 cm asset lattice — both whole numbers, so a fish placed at
a fine-lattice coordinate lands exactly on it.

(The eye column is reported as a voxel count and is only trustworthy where the
eye material differs from the marking material; on `brown-trout` both are
`skin_dark`, so that row's eye count is really an eye-plus-spots count. The
"yes/no" reading above is taken from the species where the two differ.)

What 1 cm does **not** buy is a life-sized small fish. A 10 cm anemonefish is
ten voxels, and §6's own table says three bars need at least twelve. The two
honest options are to author the small reef species at the large end of their
real size range — which is what `clown-anemonefish` and `pale-minnow` do, and
their spec notes say so — or to introduce a 5 mm tier, which nests 20:1 in the
terrain and would be a *third* asset lattice. That is an owner decision, not a
generator decision.
