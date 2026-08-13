# Eleven plumage materials: what colourful birds need from the engine

**ACCEPTED AND APPLIED, 2026-08-13.** The owner's words were "I accept all bird
colours", and all eleven landed exactly as specified below: ids 36–46,
`kMaterialCount` 36 to 47. This document is kept as the argument for each entry
rather than rewritten as a changelog — the numbers in it are what is in the
header, and the reasoning is what a future append should be held to.

**What actually landed, and one file this document got wrong.** Six files, not
five, plus the two regenerated artifacts. The missing one is
`ue-project/Tools/terrain_palette.py`, which the fish proposal missed as well
and which stopped that append's generator dead — it refuses to write anything
until every new material has an explicit `BIOME_TINT` decision. It fired here
too, naming all eleven:

    terrain_palette.py is out of step with the enum.
      missing rows (add them, with a BIOME_TINT decision): ['MAT_PLUME_WHITE', ...]

That is the guard working, and it is worth stating that it fails BEFORE writing
either artifact, so a half-done append cannot leave the shader regenerated and
the UE table stale. All eleven are `BIOME_TINT = False`, for a stronger version
of the reason the fish skins are: a biome tint is a property of a PLACE, and a
bird is the least attached thing in the library.

**What is still not verified here.** Nothing on this side compiles C++. The
enum, the palette table, the mine-cost array and the count assertion were edited
to be consistent by inspection and are checked by the two generators, which read
the header and refuse on any disagreement — but the three `static_assert`s and
the `CHECK_EQ` only actually fire under a build, and no build was run.

## The short version

The brief was **"colourful and stylised"**, and there is a measurement behind
why that is not simply a preference. Delhey 2015 took **46,559 reflectance
spectra over 17 standardised plumage patches on 555 species** and found:

| Mechanism | Share of the plumage AREA | Share of the colour GAMUT |
|---|---|---|
| **Melanin** | **74%** | **7%** |
| Carotenoid | 12% | 21% |
| **Structural** | **7%** | **45%** |
| Psittacofulvin | 3% | 18% |

**Three quarters of a bird's surface is melanin and melanin occupies seven per
cent of the colour space.** A palette weighted by area — which is what copying
a field guide gives you — is browns and greys, and a library built that way puts
a raven, a pigeon and a sparrow on a sheet as three grey-brown lozenges. The
colours that make birds *look* like birds are rare, small and mostly structural,
so a stylised library has to over-weight them deliberately.

**Eleven new entries are proposed, ids 36–46.** They are named for what they
look like rather than for what wears them, because birds are the second animal
here and will not be the last. Four are neutrals the fish set has no equivalent
for; six are saturated hues; one is keratin.

**The twenty species fill 160 material slots between them: 92 of those (58%)
are one of the eleven proposed here and 68 (42%) are one of the ten fish skins
the engine already has.** Six of the ten skins are used — `skin_dark` by all
twenty, `skin_yellow` by eight, `skin_brown` and `skin_orange` by three each,
`skin_olive` and `skin_blue` by one each — and four (`skin_pale`,
`skin_silver`, `skin_red`, `skin_green`) are not used at all, because the
plumage entries are better at what they do. That 58/42 split is why the ask is
eleven and not twenty-five: the existing set does nearly half the work.

The append costs **five hand-edited files and two regenerated artifacts** — six
hand-edited, as it turned out; see the correction at the top. Every one of them
is guarded: three by a `static_assert` or a test that fails loudly, one by a
`--check` mode, one by a generator that refuses, one by the asset-forge
selftest. **There is no site that fails silently.**

## Why not use the materials that already exist

### The four neutrals

| Wanted | Nearest existing | Why it does not do |
|---|---|---|
| true white | `MAT_SKIN_PALE` (232,226,212) | It is a fish belly: cream, and warm. Against `plume_grey` a true white measures a WCAG contrast ratio of **2.55**; `skin_pale` against the same grey measures **2.14**. That difference decides whether a gull's white head separates from its own grey mantle at twenty voxels. Three species here are essentially white animals. |
| neutral grey | `MAT_SKIN_SILVER` (176,186,196) | Closest in hue, and it is the **one entry in the whole palette whose faces are deliberately inverted** — its sides are brighter than its top, because a fish flank is a mirror reflecting bright water sideways. On a bird's back that lights the wrong surface: a bird's back is what the sun reaches and its flanks are what sits in shadow. |
| dark cool grey | `MAT_BEDROCK` (68,68,74) | The palette has no dark cool grey at all, and bedrock is terrain: patch strength 30 over a 22-voxel wavelength, so a heron's whole back would carry one slow mottle. |
| sandy tan | `MAT_SAND` (214,192,140) | Right in hue and it is sandstone — the granular material the desert rocks are made of, at the top of the jitter range. |

### The six hues

Every pair below is quoted as its **WCAG contrast ratio**, because that is what
`tools/birdprobe.py --read` gates on and because value separation, not hue, is
what makes a marking read at this size. (The eye carries brightness at roughly
four times the spatial bandwidth it carries red-against-green — the same fact
that makes chroma subsampling invisible in video.)

| Wanted | Nearest existing | Why it does not do |
|---|---|---|
| chestnut / rufous | between `SKIN_ORANGE` (226,118,34) and `SKIN_BROWN` (110,82,52) | The gap between those two is where half the small birds in the world live: a robin's breast, a kestrel's back, a swallow's throat, a jay's body, a kingfisher's underparts. Authored in orange they read as plastic toys; in brown they vanish into the branch. **Four of the twenty species use it** and it is the deepest of the six hues into the set. |
| bright scarlet | `MAT_SKIN_RED` (170,46,40) | A dark brick, and the right red for a fin. What decides it is what a red sits NEXT to: against `skin_dark` — which is a macaw's face and a woodpecker's back — `plume_crimson` measures **2.53** and `skin_red` measures **1.96**, below the 2.0 floor the probe gates on. The brick red merges into the black and the bird loses its red. |
| yellow-green | `MAT_SKIN_GREEN` (58,148,92) | A mid forest green at relative luminance **0.229**. A parrot, a greenfinch and a bee-eater are a yellow-green at **0.494** — 2.2× as bright, and the only green in the set light enough to carry a dark marking on top of it. |
| electric turquoise | `MAT_SKIN_BLUE` (46,96,168) | A royal blue at luminance **0.118**. A kingfisher, a roller and a macaw's wing are turquoise at **0.395** — a different hue and 3.3× the brightness. This is the most stylised entry in the set and it is the one the kingfisher exists for. |
| violet | `MAT_LEAF_BLOSSOM` (226,168,190) | **The palette has no violet at all.** Blossom is a pale pink for cherry trees. A jay's wing coverts, a roller's breast and the purple half of a starling's or a pigeon's gloss have nowhere else to go. Against `plume_slate` it measures 2.21. **Two species use it and it is the second entry to cut** — see below. |
| iridescent dark | `MAT_SKIN_GREEN` darkened | A saturated near-black teal is not a dark green; it is a different colour. On a starling it is the single most recognisable thing about the animal at any size, and it also serves a raven's gloss, a mallard drake's head and a magpie. |

### And the keratin

A bill and a pair of legs are two to six voxels and they are a **different
substance** from feather. Every field guide prints bill and leg colour because
it is diagnostic — and there is a stronger argument than that. Michael Azzi's
*Pixel Logic*, on Super Mario World:

> "Swoopers … are bats. However their nose was coloured orange, which makes it
> look like a bird with a beak."

**An orange protrusion in the head position converts a bat into a bird.** Not
silhouette — colour, on the beak. Minecraft gives its chicken's beak its own
box; shipped 16×16 sprite packs describe themselves as "blue body, yellow beak,
reduces down to 3 colours nicely". The bill is the cheapest identifying feature
a bird has, and drawing it in the head colour throws that away for nothing.
Yellow and orange bills use `skin_yellow` and `skin_orange`, so this proposes
only the grey one.

**The pattern is not only hue.** Every palette entry carries four numbers
besides its colours — per-voxel lightness jitter, per-voxel warm/cool tilt,
patch strength and patch wavelength — and terrain wants all four high because
terrain is granular. **A bird is one smooth creature.** That is the real reason
a leaf or a sand colour cannot be reused even where the hue matches: at jitter
58/255 a wing comes out as television static.

## The eleven entries

Colours are sRGB, the four numbers are in 1/255ths, and the format is the one
`vxc::MaterialAppearance` already uses. All eleven propose **low jitter, low hue
tilt, low patch strength and a short patch wavelength** — the quiet end of every
range, the same end the ten fish skins asked for, and the opposite end from
foliage and gravel.

| Id | Name | Top | Side | Bottom | jitter | hue | patch | scale |
|---|---|---|---|---|---|---|---|---|
| 36 | `MAT_PLUME_WHITE` | 246,246,242 | 240,240,236 | 230,230,226 | 10 | 4 | 8 | 8 |
| 37 | `MAT_PLUME_GREY` | 150,156,164 | 145,151,159 | 134,140,147 | 14 | 6 | 12 | 8 |
| 38 | `MAT_PLUME_SLATE` | 78,92,112 | 75,88,107 | 69,81,99 | 14 | 6 | 12 | 8 |
| 39 | `MAT_PLUME_BUFF` | 208,176,118 | 201,170,114 | 186,158,106 | 16 | 8 | 14 | 8 |
| 40 | `MAT_PLUME_RUFOUS` | 180,84,36 | 174,81,35 | 161,75,32 | 16 | 8 | 12 | 8 |
| 41 | `MAT_PLUME_CRIMSON` | 208,40,56 | 201,39,54 | 186,36,50 | 14 | 6 | 10 | 8 |
| 42 | `MAT_PLUME_LIME` | 152,202,70 | 147,195,68 | 136,181,63 | 14 | 8 | 10 | 8 |
| 43 | `MAT_PLUME_CYAN` | 52,184,206 | 50,178,199 | 46,165,185 | 14 | 6 | 10 | 8 |
| 44 | `MAT_PLUME_LILAC` | 166,132,208 | 160,128,201 | 149,118,186 | 14 | 8 | 12 | 8 |
| 45 | `MAT_PLUME_IRIDESCENT` | 28,78,70 | 34,88,80 | 24,68,62 | 16 | 14 | 16 | 6 |
| 46 | `MAT_BEAK_HORN` | 96,88,76 | 93,85,74 | 86,79,68 | 12 | 6 | 10 | 6 |

**Two entries break the convention on purpose, and both for the same physical
reason `MAT_SKIN_SILVER` does.**

`MAT_PLUME_IRIDESCENT` has **brighter sides than its top and the highest hue
tilt in the whole proposed set (14)**. Structural colour is not a pigment: it is
a thin-film interference effect whose hue depends on the angle between the
viewer, the feather and the light (Simpson & McGraw 2018 found that "male
position relative to the sun was the strongest predictor of colour
appearance"). A flat dark green renders a starling as a dark blob. Letting the
side faces run brighter and warmer than the top is the cheapest available
approximation of a colour that changes as you walk past it, and it costs
nothing at runtime because these four numbers already exist on every material.

`MAT_PLUME_WHITE` has the **lowest jitter in the table (10)** and the lowest
patch strength (8), lower even than the fish skins. A white bird is white
everywhere, and any per-voxel noise on it reads as dirt rather than as texture.

`kMaterialCount` goes from **36 to 47**.

## What the append touches

**1. `voxel-core/include/voxelcore/core.h`** — eleven enum lines after
`MAT_SKIN_GREEN = 35`, under a new `--- PLUMAGE MATERIALS ---` banner. The
existing `--- ASSET MATERIALS ---` banner is parsed by
`gen_material_palette_ush.py` to find where asset materials begin, and it stays
where it is: plumage materials are asset materials as far as the renderer is
concerned, exactly as the creature materials are, and the terrain amplifier
never emits them either.

**2. `voxel-core/include/voxelcore/materialpalette.h`** — eleven rows, **in id
order**. The table is positional and nothing in C++ enforces that — the
`static_assert` at the bottom counts entries and is blind to their order. The
header's own comment records what happened last time: the table was first
written grouped by type, which put `MAT_BARK_PALE` up with the other woods and
shifted every id above it, and **every broadleaf in the library rendered in
birch bark**. `asset-forge/tools/gen_palette.py` checks the order against the
enum and refuses to generate if they disagree.

**3. `ue-project/Source/VoxelEarth/VoxelAgentSubsystem.cpp:64`** — eleven
entries in `kMineCostByMaterial`. Guarded by the `static_assert` at line 128, so
omitting them is a compile error rather than a silent zero-fill.

Proposed costs: **1** for all eleven, the same as the fish skins. A bird is a
detail entity, not terrain; if an agent ever meets one it should pass through
it, not dig it. Zero is reserved for `MAT_AIR` and negative is the impassable
sentinel, so 1 is the "barely there" value — below foliage's 4–6.

**4. `voxel-core/tests/test_assetgrid.cpp:156`** — `CHECK_EQ(int(kMaterialCount),
36)` becomes 47, and the trailing comment becomes `through MAT_BEAK_HORN = 46`.
This test is doing its job: it is the tripwire that makes the count change
deliberate, and it has already caught one append.

**5. `ue-project/Shaders/VoxelMaterialPalette.ush`** — regenerated, not edited:

    python ue-project/Tools/gen_material_palette_ush.py

`VOXEL_MATERIAL_COUNT` goes 36 → 47. Forgetting to regenerate is caught by
`gen_material_palette_ush.py --check`, which needs no Unreal and no editor.

**5b. `ue-project/Tools/terrain_palette.py`** — eleven rows, each with a
`BIOME_TINT` decision, which is the one thing in that file a generator cannot
supply. **This is the entry the fish proposal did not have and should have.**
The RGB columns beside the decision are written by the same generator as the
`.ush`, from the same header, so the only hand-authored thing on each row is the
`True`/`False`. All eleven are `False`.

**6. `asset-forge/forge/palette.py`** — regenerated:

    python asset-forge/tools/gen_palette.py

Forgetting is caught by `python -m forge.cli selftest`, which re-runs the
generation in memory and compares. **`forge/materials.py` reads its own
`ENGINE_MATERIAL_COUNT` from `palette.py`** rather than carrying a literal, so
nothing needs hand-editing there and nothing can go stale — a literal in that
position was wrong twice inside one week.

### What does NOT need touching

- **`voxel-core/include/voxelcore/mips.h:60-77`** — `int counts[kMaterialCount]`
  sizes itself.
- **`voxel-core/tests/test_mesher.cpp:123-132`** — the golden digests are
  deliberately pinned to a literal 14 rather than to `kMaterialCount - 1`,
  precisely so that appending a material does not silently invalidate them.
  That trap was disarmed before the tree append.
- **`voxel-core/include/voxelcore/pathfind.h:255`** — sizes itself.
- **`assetgrid.h:178`** — `materialsWithinEngine()` gates an asset on
  `maxMaterialId() < kMaterialCount`. **Until the append lands this is what
  refuses every bird**, which is the correct behaviour and is exactly the
  position the trees and the fish were both in.

## What it costs at runtime

Nothing measurable. `kMaterialPalette` goes from 36 × 16 bytes to 47 × 16 bytes
— **176 bytes**. `kMineCostByMaterial` gains 44 bytes. The `.ush` gains eleven
constants. `mips.h` counts over eleven more slots per mip cell, on an array that
is already stack-allocated per cell.

## How it was measured, and the gate it has to pass

`tools/birdprobe.py --read` computes the WCAG contrast ratio between each
species' marking and the thing it sits on — **four of them, not one**. Three
are the marking regions, which sit on three different base colours: a single
check against a single base colour passed every species in the library and
would have shipped a white wing bar on a white ptarmigan.

**The fourth is the bill against the head, and it was the most productive check
in the file.** It was added last, and when it ran, **ten of the twenty species
failed it**: a robin's horn-coloured bill on its olive head measured **1.04**, a
great tit's black bill on its black cap measured **1.00**, a raven's and a
swallow's measured 1.40, a heron's and a gull's yellow bills on their white
heads measured 1.59. The bill was present in the voxels on all ten and invisible
in every render. Twelve of the twenty now carry a bright bill — eight yellow,
three orange, one white — and three more a grey one, which is what the *Pixel
Logic* result above says they should.

**The floor is 2.0, where the fish probe's is 1.5**, and the difference is the
brief. At 1.5 a marking is present and faint; at 2.0 it is a block of colour.
For reference, Mojang did not gate their tropical fish on contrast at all: 16%
of their random dye pairs come out under a ratio of 1.2, and one shipped preset
at 67% ink measures 1.04 and genuinely renders as a plain blue blob.

Against the colours proposed above, **all twenty species pass with no flags**,
measuring **2.18 to 12.14**.

One species was genuinely under the floor and was fixed by changing a colour,
which is what a gate is for: `barn-swallow`'s chestnut throat on its near-black
head measured **1.90**, and its throat became orange, which measures 3.07.

**Two of the other early failures were the instrument, not the birds, and they
are worth recording because both lied in the reassuring direction.** A herring
gull reported **74% ink** on a species that carries no marking at all — its
head-marking slot happens to hold the same grey its back is painted in, and the
probe counted every voxel of it. A common buzzard then reported **0.0% ink** on
a bold white wing panel, because the fix for the gull discarded any marking
whose colour matched another slot and the buzzard's panel and its underparts are
both white. Ink is now measured as the difference between the bird and the same
bird with its markings turned off, both builds with variation pinned so they are
the same individual. That measures the marking whatever colour it shares.

## If eleven is too many

The set can be cut to **five** and still carry all twenty species, at a cost
worth stating plainly. The cut order below is by **measured usage across the
twenty specs**, cheapest first:

| Entry | Species using it | Folds back to | What is lost |
|---|---|---|---|
| `plume_lime` | **1** (great tit) | `skin_green` | the one yellow-green in the library; a great tit's back becomes forest green |
| `plume_lilac` | **2** (raven, pigeon) | `plume_slate` | the only violet; both glosses become grey |
| `plume_crimson` | **3** (macaw, woodpecker, ptarmigan) | `skin_red` | the woodpecker's nape and the ptarmigan's comb drop to 1.96 contrast against the black they sit on, under the 2.0 gate |
| `plume_cyan` | **3** (kingfisher, jay, mallard) | `skin_blue` | the kingfisher becomes a dark blue bird rather than a turquoise one |
| `plume_iridescent` | **4** (starling, raven, swallow, mallard) | `skin_dark` | four species become the same black |
| `plume_rufous` | **4** (kestrel, kingfisher, jay, thrush) | `skin_brown` | four species lose their chestnut and read as brown |

Everything below that line is load-bearing: `plume_white` is in **15** of the
twenty, `plume_slate` in 8, `plume_buff` in 7, `beak_horn` in 6 and
`plume_grey` in 5.

**The floor is five: white, grey, slate, buff and horn.** Those five plus the
existing skins are what every neutral-coloured species in the library is made
of, and there are fourteen of them. Below that the set stops being able to draw
a bird at all rather than merely a duller one.

**The recommendation is eleven.** The whole set is 176 bytes, and the difference
between eleven and five is the difference between a library that has a
kingfisher, a macaw and a starling in it and one that has three more brown
birds.

## How to check it landed

    python asset-forge/tools/gen_palette.py
    python ue-project/Tools/gen_material_palette_ush.py --check
    python -m forge.cli materials        # every plume should stop saying PROPOSED
    python -m forge.cli selftest
    python asset-forge/tools/birdprobe.py --read

The last one is the one that matters for colour.
