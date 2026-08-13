# Ten creature-skin materials: what fish colour needs from the engine

> **ACCEPTED AND LANDED (owner, 2026-08-13).** All ten are in the engine:
> `MAT_SKIN_DARK` = 26 through `MAT_SKIN_GREEN` = 35, and `kMaterialCount` went
> 26 → 36. The eleven bird plumage materials were appended after them at 36–46,
> so the count is **47** as of this writing — check the header, not this
> document, for the current value.
>
> **The cost below is wrong, and how it was wrong is the useful part.** It says
> five files. There were six. `ue-project/Tools/terrain_palette.py` refuses to
> generate until every new material has an explicit `BIOME_TINT` decision, and
> it stopped the generator dead — the proposal had not found it. That is a
> guard doing exactly its job, and the bird proposal that followed hit the same
> one because it had been written from this document. Anyone costing the next
> append should start from the guards, not from this list.
>
> Still owed: **no C++ has been compiled.** The `static_assert`s and the
> `CHECK_EQ` in `test_assetgrid.cpp` only fire under a build. Every table has
> been parsed and checked against every other by hand and by `--check`.

The generator is written against these ten names. Before the append they worked
in asset-forge as stand-ins, and nothing authored with them could be stamped
into the world; `AssetGrid::materialsWithinEngine` refused them, correctly.

## The short version

A fish's species identity at twenty voxels is carried mostly by colour, not by
outline. The engine's palette is terrain-oriented and has no orange, no silver
and no reef blue, so **ten new entries are proposed**. They are named for what
they look like rather than for what wears them, because fish are the first
animal here and will not be the last — a frog is `skin_green`, a trout's back is
`skin_olive`, a clownfish and a goldfish are both `skin_orange`.

The append costs **six files and two regenerated artifacts** (this said five and
one when it was written; see the banner above). Every one is guarded: three by a
`static_assert` or a test that fails loudly, one by a `--check` mode, one by the
asset-forge selftest, and `terrain_palette.py` by refusing to write at all until
each new material has a `BIOME_TINT` decision. There is no site that fails
silently — which is why the missing sixth file was found in seconds rather than
shipping as a wrong colour.

## Why not use the materials the engine already has

Six terrain materials come close enough to be worth listing, and each is wrong
in a way that shows on a twelve-voxel flank:

| Wanted | Nearest existing | Why it does not do |
|---|---|---|
| pale belly | `MAT_SNOW` (243,246,251) | voxel jitter 10 and patch strength 16 — the lowest in the palette. A belly drawn in it reads as a printed decal rather than as an animal. |
| silver flank | `MAT_PERMAFROST` (170,180,188) | right hue, but it is frozen ground: patch scale 22 voxels, so a 26-voxel fish gets one slow mottle across its whole side. |
| orange | `MAT_LEAF_AUTUMN` (192,118,50) | the only orange in the engine, and it is a brown-orange with voxel jitter 62 and hue jitter 48 — the highest in the table. On a flank that is static, not skin. |
| olive back | `MAT_SAVANNA_GRASS` (170,158,90) | too yellow, and its bottom face is soil brown, so a fish would change colour when seen from below. |
| brown | `MAT_SUBSOIL` (120,96,72) | plausible, and the closest single match in the whole palette. |
| dark marking | `MAT_BEDROCK` (68,68,74) | plausible. |

So two of the ten could be borrowed. The other eight cannot, and a species set
built from two borrowed materials plus eight approximations is worse than not
shipping colour at all.

**The pattern is not only hue.** Every palette entry carries four numbers
besides its colours — per-voxel lightness jitter, per-voxel warm/cool tilt,
patch strength and patch wavelength — and terrain wants all four high because
terrain is granular. **An animal is one smooth creature.** That is the real
reason a leaf colour cannot be reused for a fish even where the hue matches: at
jitter 58/255 a flank comes out as television static.

## The ten entries

Colours are sRGB, the four numbers are in 1/255ths, and the format is the one
`vxc::MaterialAppearance` already uses. All ten propose **low jitter, low hue
tilt, low patch strength and a short patch wavelength** — the quiet end of every
range, and the opposite end from foliage and gravel.

| Id | Name | Top | Side | Bottom | jitter | hue | patch | scale |
|---|---|---|---|---|---|---|---|---|
| 26 | `MAT_SKIN_DARK` | 46,48,56 | 44,46,54 | 40,42,50 | 12 | 4 | 10 | 8 |
| 27 | `MAT_SKIN_PALE` | 232,226,212 | 226,220,206 | 216,210,198 | 12 | 5 | 10 | 8 |
| 28 | `MAT_SKIN_SILVER` | 176,186,196 | 186,196,206 | 196,204,212 | 16 | 6 | 12 | 6 |
| 29 | `MAT_SKIN_OLIVE` | 86,96,54 | 82,92,52 | 76,85,48 | 14 | 8 | 12 | 8 |
| 30 | `MAT_SKIN_BROWN` | 110,82,52 | 105,78,50 | 97,72,46 | 16 | 8 | 14 | 8 |
| 31 | `MAT_SKIN_ORANGE` | 226,118,34 | 218,113,32 | 204,105,30 | 14 | 8 | 10 | 8 |
| 32 | `MAT_SKIN_YELLOW` | 232,194,54 | 224,187,52 | 210,175,48 | 14 | 8 | 10 | 8 |
| 33 | `MAT_SKIN_RED` | 170,46,40 | 164,44,38 | 152,41,35 | 14 | 8 | 12 | 8 |
| 34 | `MAT_SKIN_BLUE` | 46,96,168 | 44,92,162 | 40,85,150 | 14 | 6 | 12 | 8 |
| 35 | `MAT_SKIN_GREEN` | 58,148,92 | 56,142,88 | 51,131,81 | 14 | 8 | 12 | 8 |

**The silver entry is the only one whose faces are deliberately non-uniform, and
it is the wrong way round on purpose**: its side faces are *brighter* than its
top. A silver fish is a mirror, and what a mirror on a vertical flank reflects
is the bright water beside it, while its top face reflects the dark bottom. Every
other entry darkens downward by about 8%, which is the convention the rest of the
table already uses.

`kMaterialCount` goes from **26 to 36**.

## What the append touches

**1. `voxel-core/include/voxelcore/core.h`** — ten enum lines after
`MAT_LEAF_AUTUMN = 25`, under a new `--- CREATURE MATERIALS ---` banner. The
existing `--- ASSET MATERIALS ---` banner is parsed by
`gen_material_palette_ush.py` to find where asset materials begin, and it stays
where it is: creature materials are asset materials as far as the renderer is
concerned (the terrain amplifier never emits them either).

**2. `voxel-core/include/voxelcore/materialpalette.h`** — ten rows, in id
order. **The table is positional and nothing in C++ enforces that** — the
`static_assert` at the bottom counts entries and is blind to their order. The
header's own comment records what happened last time: the table was first
written grouped by type, which put `MAT_BARK_PALE` up with the other woods and
shifted every id above it, and every broadleaf in the library rendered in birch
bark. `tools/gen_palette.py` checks the order against the enum and refuses to
generate if they disagree.

**3. `ue-project/Source/VoxelEarth/VoxelAgentSubsystem.cpp:64`** — ten entries in
`kMineCostByMaterial`. Guarded by the `static_assert` at line 103, so omitting
them is a compile error rather than a silent zero-fill.

Proposed costs: **1** for all ten. A fish is a detail entity, not terrain; if an
agent ever meets one it should pass through it, not dig it. Zero is reserved for
`MAT_AIR` and negative is the impassable sentinel, so 1 is the "barely there"
value — below foliage's 4–6.

**4. `voxel-core/tests/test_assetgrid.cpp:156`** — `CHECK_EQ(int(kMaterialCount),
26)` becomes 36, and the trailing comment becomes `through MAT_SKIN_GREEN = 35`.
This test is doing its job: it is the tripwire that makes the count change
deliberate.

**5. `ue-project/Shaders/VoxelMaterialPalette.ush`** — regenerated, not edited:

    python ue-project/Tools/gen_material_palette_ush.py

`VOXEL_MATERIAL_COUNT` goes 26 → 36. Forgetting to regenerate is caught by
`gen_material_palette_ush.py --check`, which needs no Unreal and no editor.

**6. `asset-forge/forge/palette.py`** — regenerated:

    python asset-forge/tools/gen_palette.py

Forgetting is caught by `python -m forge.cli selftest`, which re-runs the
generation in memory and compares.

### What does NOT need touching

- **`voxel-core/include/voxelcore/mips.h:60-77`** — `int counts[kMaterialCount]`
  sizes itself.
- **`voxel-core/tests/test_mesher.cpp:123-132`** — the golden digests are
  deliberately pinned to a literal 14 rather than to `kMaterialCount - 1`,
  precisely so that appending a material does not silently invalidate them.
  That trap has already been disarmed.
- **`voxel-core/include/voxelcore/pathfind.h:255`** — sizes itself.
- **`assetgrid.h:178`** — `materialsWithinEngine()` gates an asset on
  `maxMaterialId() < kMaterialCount`. **Until the append lands this is what
  refuses every fish**, which is the correct behaviour and is why the fish are
  in the same position the trees were.

## What it costs at runtime

Nothing measurable. `kMaterialPalette` goes from 26 × 16 bytes to 36 × 16 bytes
— 160 bytes. `kMineCostByMaterial` gains 40 bytes. The `.ush` gains ten
constants. `mips.h` counts over ten more slots per mip cell, on an array that is
already stack-allocated per cell.

## If ten is too many

The set can be cut to **six** and still carry all ten species, at a cost worth
stating: `skin_yellow`, `skin_red`, `skin_green` and `skin_blue` are what make
`reef-tang` and `river-perch` read as reef and weed-bed fish rather than as two
more brown ones. The floor is `skin_dark`, `skin_pale`, `skin_silver`,
`skin_olive`, `skin_brown`, `skin_orange` — countershading plus one bright
colour, which is enough for every freshwater species in the set and for nothing
in salt water.

The set cannot go below six. Countershading needs three (dark, mid, pale) and a
marking needs a fourth that contrasts with the flank; two more buy the
difference between a fish and a grey fish.

## How to check it landed

    python asset-forge/tools/gen_palette.py
    python ue-project/Tools/gen_material_palette_ush.py --check
    python -m forge.cli materials        # every skin should stop saying PROPOSED
    python -m forge.cli selftest
    python asset-forge/tools/fishprobe.py --read

The last one is the one that matters for colour. It computes the WCAG contrast
ratio between each species' flank and its marking and flags anything under 1.5,
because the eye carries brightness at roughly four times the spatial detail it
carries hue — a marking that differs only in hue is present in the voxels and
invisible in the water. Against the colours proposed above the ten species
measure 1.83 to 7.64, with nothing near the floor.
