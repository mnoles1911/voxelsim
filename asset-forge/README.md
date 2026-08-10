# asset-forge

Cubic-voxel environment assets for voxelsim: trees, bushes, rocks, grass, reeds
and flowers.

Why it is built the way it is: `docs/tree-asset-generator-research.md`.
What it is for and where it goes: `docs/tree-asset-generator-plan.md`.

## The idea in one paragraph

A species is a JSON file. Sliders write it, plain-language requests patch it,
batch generation reads it, a seed picks one individual out of it, the library
stores it. An asset is therefore never a blob of voxels we have to keep — it is
`(spec, seed)`, a few hundred bytes that regenerate byte-for-byte the same
voxels every time. That is what makes "hundreds of variants" cheap.

## Status

Phases 1 and 2 work: the generator core and the app. 42 species across six asset
kinds, **every one of them on the 5 cm asset lattice**. Phase 0 (making the
engine able to hold an asset at all) is deferred until the editor box is free
and is tracked in the plan.

There are **two** engine blockers, and it matters which assets need which:

- **The streaming bound** skips any chunk `surfaceUpperBoundMm` proves empty,
  which is every chunk a crown or a stem would occupy. Everything here needs
  this fixed.
- **The material append** — bark, the leaf variants and a bloom colour do not
  exist in `vxc::Material`. Trees, bushes and flowers need this.

**Rocks, grass and reeds need only the first.** A boulder is `MAT_ROCK`,
`MAT_GRAVEL`, `MAT_BEDROCK` and `MAT_SAND`; a grass tuft and a reed are
`MAT_GRASS` and `MAT_SAVANNA_GRASS`. Those are the materials the terrain is
already made of, so seventeen of the forty-two species are one blocker away from
the world instead of two. If something has to ship first, it is those.

Nothing consumes the **placement** group yet — `abundance`, `spacing_m`,
`cluster`, the slope and elevation gates and the biome weights are authored and
read by no code. Scattering has to happen in `voxel-core` during worldgen,
per-chunk and deterministic; these sliders are the specification for it, not an
implementation of it.

## Asset kinds

The app opens on a **section** per kind — Trees, Bushes, Rocks, Grass, Reeds and
Flowers. The section scopes everything below it: which
parameters exist, which species the dropdown lists, which library entries show,
and which biomes the coverage tab reports on. A rock has no trunk, crown, growth
model or foliage, and the rock section does not show those sliders greyed out —
it does not show them at all.

`forge/kinds.py` is the whole registry. Adding a kind is a generator, a
parameter group and one entry there; it is not a second application.

| Kind | Generator | Species | Needs the material append? |
|---|---|---|---|
| Trees | skeleton (`colonize` / `whorl` / `frond`) | 16 | yes |
| Bushes | same skeleton, authored short with branches to the ground | 4 | yes |
| Rocks | accretion and carving — see **Rocks** | 10 | no |
| Grass | tuft — see **Ground cover** | 4 | no |
| Reeds | tuft, tall and near-vertical, seed heads | 3 | no |
| Flowers | tuft, few stems, some carrying a bloom | 5 | yes (bloom colour) |

Biome weights are scoped per kind too, because **Bare rock** is a real place for
a boulder and an impossible one for an oak: the engine's cliff gate fires before
the climate table, so anything steeper than a 70% grade is bare rock and never
hosts a tree. That is one flag in `biomes.py`, not a special case in the UI.

## Run the app

    python -m forge.cli serve

Opens <http://127.0.0.1:8731/> in your browser. Left panel is every parameter
as a slider, grouped; the middle is a gallery of variants; click one for a
large render, its measurements and **Keep to library**.

### On a phone or tablet

    python -m forge.cli serve --host 0.0.0.0

The banner then prints a second URL with this machine's LAN address; open that
on the phone. Loopback-only is the default on purpose — the app writes files and
has no authentication — so the LAN bind is opt-in rather than assumed.

The layout is responsive rather than a separate build. Below 820 px the slider
panel becomes a drawer over the gallery (**☰ Sliders**, and it closes itself
when a change regenerates, because on a phone it covers the thing you changed);
the kind bar scrolls sideways; the detail sheet goes full-bleed. The 3D viewer
takes **drag to spin, pinch to zoom, double-tap to reset** — pinch is the zoom
on a device with no wheel, and double-tap is the reset for a thumb that cannot
reach the button. A touch client also asks the server for a smaller voxel budget
(400k instances instead of 2.5M) and the caption still says what it is showing.

`forge/web/_probe.html` checks the narrow layouts: it loads the app in a
fixed-width iframe and reports anything that runs past the edge. It exists
because Chrome on Windows will not open a window narrower than about 500 px, so
`--window-size=390` renders a wider viewport and a screenshot taken that way
says nothing about a phone. `python tools/shots.py` drives it.

- **Inspect in 3D** — click any tree and the detail view opens an orbit viewer:
  drag to spin, scroll to zoom, **Reset view** to reframe. **Flat** switches
  back to the fast isometric render. The server sends only surface voxels
  (a solid trunk's interior never leaves the machine), so a 135k-voxel oak
  arrives as ~105k instanced cubes and draws in one call. Hand-written WebGL2 —
  no library to vendor, works offline.
- **Describe a change in plain language** — the box above the sliders takes
  things like *"much shorter, gnarled, sparser canopy"* and moves the sliders
  for you. It runs **entirely in-app**: no network, no model, no API key. A
  fixed vocabulary of ~200 design phrases (`forge/language.py`) maps onto
  parameter recipes written against how the generator actually works — so
  "twiggier" becomes a smaller consumption radius plus more growth targets,
  because there is no branch-count slider to turn.

  It understands intensity (*slightly / much / extremely*), negation (*not so
  tall*, *less droopy*), silhouettes (*flat-topped*, *conical*, *columnar*),
  switches (*dead*, *leafless*, *needles*, *in blossom*) and explicit numbers
  (*height 8*, *crown radius 3*). Clause boundaries are respected, so in
  "much shorter, sparser" the *much* applies to the height only.

  Two properties matter more than breadth here. It is **deterministic** — the
  same sentence always produces the same edit, which is what you want from a
  box you will type into a hundred times. And when it does not recognise a
  word it **says so** rather than silently doing nothing; an unknown word is a
  gap in the vocabulary to fill, not a failure to debug. Edits land in the same
  spec a slider drag does, so they are validated, clamped, visible as moved
  sliders, and undoable with **Revert**.
- **Generate / More / Reroll** — a fresh batch, the next block of seeds, or a
  random seed range.
- **auto-regenerate** — regenerates when you release a slider, not during the
  drag; a tree costs a few hundred milliseconds and mid-drag frames would only
  ever be stale.
- **Keep to library** writes `library/<species>/<species>-<seed>/` containing
  the spec, the realized individual, `tree.vox`, `tree.vxa`, a thumbnail and
  measurements. The Library tab lists everything kept in the current section,
  with download links. There is a **+** on each gallery tile too — approving a
  batch is the common case (generate forty, take the six that read well) and
  routing each of those six through the detail overlay is six round trips for a
  decision already made from the thumbnail.
- **Keep all clean** keeps every tile in the gallery that generated without a
  health flag. Flagged ones are skipped deliberately: a health problem is
  exactly the case that deserves a look first, and this is the one control that
  could otherwise put a broken asset in the library forty at a time.
- **Save spec** writes the current sliders back to `specs/<name>.json`. Change
  the species name first to fork a new species instead of overwriting.
- **Library** entries each carry **Inspect in 3D**, **More like this** (loads
  that tree's approved spec and generates a fresh block of seeds from it), and
  **Delete** (removes the entry's files from disk, with a confirm).
- `#seed=N` in the URL opens that variant directly, `#kind=rock` opens a
  section, `#tab=biomes` a tab — so a particular asset or view can be
  bookmarked or handed to someone.

Finished tiles are memoised by `(spec hash, seed, scale)`. Because generation
is deterministic a tile computed once is valid forever, so returning to a
species or nudging a slider back is instant rather than a regrow.

## Command line

    python -m forge.cli gen     specs/temperate-oak.json --seed 7
    python -m forge.cli batch   specs/temperate-oak.json --count 100
    python -m forge.cli survey                    # every species on one page
    python -m forge.cli check   specs/tundra-pine.json
    python -m forge.cli schema                    # the slider table, as JSON
    python -m forge.cli materials                 # material IDs and engine status
    python -m forge.cli selftest

Needs `numpy`, `scipy`, `pillow`. The app itself adds no dependencies — it is
stdlib `http.server` and a hand-written page. Output lands in `out/`.

`batch` writes a contact sheet: every seed on one page, each labelled with its
seed and measured height, anything the health check flagged marked in the
corner. That sheet is the actual product — choosing is the designer's job and
it happens by eye.

## Voxel size

Each species carries its own **Voxel size** (the `resolution_cm` slider), and
**every species is set to 5 cm** — the asset lattice. 10 cm is the terrain's own
size; 2.5, 2 and 1 cm remain selectable for hero renders and comparisons but
nothing authored there can be put in the world. See *Two lattices* below for why
5 cm and not 2 cm.

**The cost is cubic**, which is the whole reason the choice matters — halving
the voxel is 8× the voxels, and 2 cm is 125× the voxels of 10 cm for the same
tree. Measured on the species set:

    temperate-sapling   10 cm      8,026 vox   0.1 s        2 cm    981,091 vox   0.5 s
    tundra-pine         10 cm     25,406 vox   0.2 s        2 cm  3,107,527 vox   1.4 s
    temperate-oak       10 cm    108,556 vox   0.4 s        2 cm 13,378,565 vox   5.4 s
    jungle-emergent     10 cm    353,000 vox   0.6 s        2 cm 54,007,589 vox  42.3 s

So the app keeps three resolutions separate, and says which one you are looking
at rather than leaving it ambiguous:

- **Gallery tiles** build at the finest size whose grid fits a cell budget
  (`ASSET_FORGE_PREVIEW_CELLS`, default 6M). A 14 m oak lands on 10 cm; a 45 cm
  grass tuft lands on 5 cm. The skeleton is resolution-independent, so a coarse
  preview and a fine export are the same asset sampled differently.

  This used to be a flat 10 cm floor, which was right for trees and badly wrong
  for everything small — a 1 m bush at 10 cm is ten voxels tall, so every shrub
  in the gallery previewed as an unreadable smudge no matter how it was
  authored. Cost is cells, so budgeting cells gets both ends right from one
  rule.
- **The 3D viewer** shows the finest size that fits its instance budget
  (`ASSET_FORGE_MAX_VIEWER_VOXELS`, default 2.5M; a touch client asks for 400k).
  Small species show at their authored size; a species that has to be stepped
  coarser says so in the caption — *"shown at 10 cm (exports at 5 cm)"*.
- **Exports** (`.vox`, `.vxa`, Keep to library) always use the authored size.

### Two lattices: 10 cm and 5 cm

The engine has exactly one voxel size today — `kVoxelSizeMm = 100` in `core.h`,
and nothing in voxel-core references a finer one. The plan is a second, finer
tier near the surface, and **that tier is 5 cm**.

5 cm rather than 2 cm because of how it nests. 5 cm is 2:1 inside the terrain
lattice: eight fine voxels per coarse one, a single subdivision level, trivial
mip. 2 cm is 5:1 at 125×, which makes bricks, LOD and the digging boundary all
messier, and costs about fifteen times as much.

What it costs in detail was measured, not guessed
(`tools/lattice_ab.py` renders each plant authored twice, once for each size):

- **Reeds lose almost nothing.** A 2 m stem has voxels to spare either way.
- **Grass changes character** — it stops being individual blades and becomes a
  chunky vegetation clump. Legible, but a different look.
- **Flowers were the real constraint.** A bloom two voxels wide had nowhere to
  put a petal, so the rosette collapsed back into the blob it replaced. Fixed by
  drawing small blooms *on* the lattice instead of sampling onto it: one centre
  voxel and four cardinal petals. That is what lets 5 cm carry a daisy rather
  than only a sunflower.

So: **terrain at 10 cm, every asset at 5 cm.** One size for all assets, so
nothing downstream ever has to ask which lattice a given object lives in. The
1 cm and 2 cm options stay in the app for hero renders and A/B comparisons;
nothing authored below 5 cm can be put in the world.

Moving everything down was not a resolution swap. Two things had been tuned
against a 2 cm voxel and mean something else at 5 cm: a `growth.tip_radius_m` of
0.02 m is under half a voxel, so twigs get drawn at the one-voxel minimum; and a
trunk radius of 0.05 m is one voxel, which is not enough for a shrub's main
stems to read as stems. `tools/retune_5cm.py` and `tools/all_to_5cm.py` record
what changed and why.

**What the 8× costs.** The sixteen large species went from 866k voxels to 6.7M
between them. Build times stay workable — most trees are one to two seconds, the
30 m emergent about five, and the 9 m `summit-tor` is the worst in the library
at eight to seventeen. Previews are unaffected, because gallery tiles and the 3D
viewer already pick their own resolution against a budget: a 30 m tree still
previews at 10 cm and only an export pays full price.

The visible consequence is `.vox` **splitting**, which is now the normal case
rather than the exception. The format caps a model at 256 voxels per axis, so
most trees are written as a scene of several — the emergent is nine. The writer
has always done this and the selftest checks the round trip; what changed is
that it stopped being worth flagging. See *Health checks*.

### What going fine exposed

Two bugs sat harmlessly in the foliage code for as long as everything was 10 cm,
and both showed up as thousands of loose voxels the moment a clump was tens of
voxels across instead of two. They were found at 2 cm, before the tier settled
on 5 cm; both fixes are resolution-independent and both still matter at 5 cm:

- **A clump could come off its twig entirely.** Droop and jitter are world
  distances, so their ratio to the clump radius is the same at any resolution —
  but at 10 cm a clump is two voxels wide and any near miss still touches, while
  at 2 cm the same displacement left whole thousand-voxel leaf masses floating
  in mid air. Clump displacement is now capped at three quarters of its radius,
  which keeps every clump overlapping its anchor and leaves droop its visible
  range.
- **`foliage.density` shattered the clump.** Dropping a third of the voxels
  independently is fine on a two-voxel ball and turns a fifty-voxel one into TV
  static. It now removes coherent patches (four detuned sine waves) and the
  slider means the exact fraction kept.

The health check that caught both is `attached`, and it is the reason to run
`tools/healthpass.py` over every species after any change to rasterization.

`pipeline.build` refuses a grid over `ASSET_FORGE_MAX_GRID_MB` (default 3 GB)
before allocating, with the dimensions and the way out, rather than thrashing
the machine. Nothing that ships comes near it now that the fine tier is 5 cm —
the 28 m emergent is 350k voxels at 10 cm — but a 2 cm hero render of that tree
needs 1.8 GB and 42 s, which is the ceiling of what dense storage handles.

From the command line: `python -m forge.cli gen specs/tundra-pine.json --res 5`.

## Canopy structure

The single biggest lever on whether a broadleaf reads as a tree or as broccoli
is **`foliage.separation`** — the minimum distance between clump centres, as a
multiple of clump radius.

This was found by putting our output beside treegen-pinegen's published sample
renders (`out/vs-treegen.png`). Their crowns break into distinct masses with
daylight between them and the branch architecture visible through; ours filled
the entire crown envelope into one continuous solid. The cause was placing a
clump on every qualifying twig at 85-98% coverage with a radius large enough to
overlap its neighbours, so the union was always a shell.

Raising coverage or lowering it does not fix this. Twigs are dense, so any
coverage high enough to fill the crown also packs the clumps until they fuse.
The fix is to thin candidates by *distance* before the coverage roll — greedy
dart-throwing with a spatial hash — so clump centres keep their distance
whatever the twig density.

On a 14 m oak the range runs from 405 clumps at separation 1.0 (a solid mass)
to 27 at 3.0 (bare branches with pom-poms). Around **1.9 with a clump radius of
0.8 m** the crown keeps its volume while gaining gaps and showing its boughs,
which is the target. Conifers want less (1.4-1.5, so tiers stay dense),
a weeping willow much less (1.3, its curtain should read solid), a sparse birch
more (2.0).

## Growth models

One algorithm could not make every tree. Species differ in *structure*, not
just in parameter values — a conifer's tiers, a palm's fronds and a willow's
strands are three different things, and expressing them all as "small ragged
spheres on twigs" is why our palm was a lollipop and our willow was a ball.
`growth.model` picks the backend; two post-passes add structure any backend can
carry.

**`colonize`** — space colonization (Runions et al. 2007). Branches grow toward
scattered targets and consume them on arrival, so competition for space
produces the pattern. Right for irregular broadleaf crowns; it is the default
and still what most species use.

**`whorl`** — rings of branches up a straight leader, each arcing out with lift
at the trunk and droop at the tip, plus forward-angled side shoots. This is
what a conifer *is*: a ring laid down each growing season. Colonization can
only make a crown that is cone-*shaped*, never one that is actually tiered.
Used by `tundra-pine` and `columnar-cypress`.

**`frond`** — an unbranched trunk carrying a crown of long arcing leaves. A
palm has no branches at all, so the blade gets its own rasterizer: a squashed
ellipsoid at every midrib node, widest a third of the way out and tapering to a
point at both ends. Used by `coast-palm`. Blades must be narrower than their
spacing or the crown closes into a disc — twelve fronds at 0.34 m reads as a
palm, fourteen at 0.55 m reads as a plate.

**Strands** (`strand.count`) — long thin branches that ignore targets and
simply fall. This is the one thing colonization structurally cannot do, and it
is exactly what a weeping willow is; the same pass gives lianas and hanging
moss. Anchors are biased toward the crown edge so strands form a curtain rather
than a beard down the middle. Used by `weeping-willow`.

**Roots** (`roots.count`) — ridges arching out of the base and back down to the
ground. Distinct from `trunk.buttress`, which only multiplies the trunk radius
near the ground: that thickens a cylinder, it does not make a root. Used by
`jungle-emergent`, `baobab` and `temperate-oak`.

Growth model and silhouette stay separate: every model reuses the same
`crown.shape` profiles, so `tundra-pine` is whorl + cone (tiered spruce) and
`columnar-cypress` is whorl + column (narrow evergreen) from the same code.

## Rocks

A rock has no skeleton, so it does not use the growth pipeline at all
(`forge/rock.py`). Four steps:

1. **Mass** — ellipsoids unioned as a *field*, then the surface pushed in and
   out by low-frequency noise before it is thresholded into voxels.
2. **Faceting** — half-space cuts, placed by quantile of the remaining mass.
3. **Erosion** — coherent patches off the most exposed places; a light finish.
4. **Burial** — everything below z=0 cut away, so it looks settled, not dropped.

Three things there were each arrived at by discarding an obvious version:

**Roughness is the whole ballgame, not a detail pass.** Unioning ellipsoids
straight into voxels gives a perfectly smooth surface, and a smooth curve at
twenty-odd voxels across shows its stair-steps as clean concentric contour rings
— the exact look of a Minecraft sphere. No amount of cutting or eroding removes
those rings; only breaking the surface before it becomes voxels does. `rock.rough`
is the single parameter that decides whether the thing reads as stone.

**Facets are placed by quantile, not by depth.** "Cut at 80% of the reach"
sounds equivalent to "take 20% off" and is not: an ellipsoid's far point along a
normal is much further out than its bulk, so depth-as-a-fraction shaved a cap of
a few dozen voxels and the silhouette stayed an egg. A quantile says what it
means, so `rock.angular` reads directly as how blocky the result is. Normals
within ten degrees of an axis snap onto it — a cut plane a few degrees off axis
lands as a long shallow staircase of parallel ridges, which read as machining
marks across the top of every rock and survived every attempt to fix them by
changing the noise.

**`rock.size_m` is measured, not estimated.** Every step takes mass away, so the
raw lumps must start larger. A formula tuned so a boulder came out right left a
3.2 m standing stone at 4.9 m, because the burial cut removes a different share
of a tall stone than of a flat one. The builder now measures the result and
corrects on the same seed — searching on a coarse copy first, because the
correction is a ratio of lengths and barely depends on the lattice, so a 9 m
boulder is not built three times at full size to find it.

### What the large boulders exposed

The 4.5-9 m boulders are a size class above anything else and they broke two
things that were invisible at 2-3 m:

**Relief has to be measured in voxels, not only in rock-fractions.** The surface
noise had two octaves sized to the rock, so a 9 m boulder got the same six
undulations a 2 m one did — and between those six, the surface was still a
smooth curve showing exactly the concentric stair-steps the whole pass exists to
prevent. A third octave a couple of *voxels* wide fixes it at every size,
because terracing is a property of the lattice and not of the stone.

**A fracture face is not a plane.** Faceting cut true half-spaces, which on a
6 m block leaves flat faces metres across; the first large boulders read as cut
gemstones rather than stone. The cut plane now wobbles by a few voxels of noise,
so the depth still means what the quantile says while the surface it leaves is
broken. It reuses the same noise field the body does — generating a second one
doubled the slowest part of the build for a difference nobody could see, and a
fracture following the same grain as the weathering is if anything more
physical.

## Ground cover

Grass, reeds and flowers are one generator with three settings, not three
generators (`forge/ground.py`). A grass tuft, a stand of reeds and a clump of
daisies are all *a spray of thin stems rising from a common root*; what
separates them is how tall the stems are, how far they arc over, and what sits
on top — nothing, a seed spike or plume, or a bloom. That is not a shortcut, it
is the actual shape, and it is why `tuft.head` is the parameter that tells the
three kinds apart.

None of it touches the tree machinery. A blade of grass does not branch, does
not compete for space and carries no foliage clumps, so space colonization has
nothing to contribute.

**All twelve are authored at 5 cm**, the fine lattice tier (see *Two lattices*
above). At the terrain's 10 cm a 45 cm tuft is four voxels and there is nothing
to draw; at 5 cm it is nine, which is enough for a shape.

Authoring *for* 5 cm is a different job from shrinking a 2 cm asset into it, and
three rules came out of doing both and comparing:

- **Fewer, wider stems.** At 5 cm a blade is one voxel wide whatever you ask
  for, so thirty-four of them rooted in a 5 cm disc land on top of each other
  and the tuft fuses into a plate. Ten to fifteen, spread wider, is what the
  lattice can express.
- **Taller.** Pushing ground cover up a little buys the shape somewhere to
  exist; the alpine sedge went from 16 cm to 30 cm for exactly that reason.
- **Heads have to clear a couple of voxels** to read as anything.

Per-area cost at 5 cm, at each species' own authored spacing: grass is
26k-64k voxels per 100 m², flowers 0.4k-17k, and reeds the expensive ones at
26k-316k. A whole temperate oak is about 75k, so a 10 × 10 m reed bed is four
oaks and a 10 × 10 m meadow is under one.

Four things had to be got right, and each was wrong first:

**A flower head is a rosette, not a ball.** A bloom is two or three voxels
across; a sphere at that size has no roundness left to lose, and squashing one
gave a flat rectangular slab — a pink plate on a stick. Petals placed on a ring
keep a lobed silhouette the eye reads as a flower.

**Below two voxels of radius, even the rosette collapses.** The ring itself
comes out under one voxel wide, so every petal rounds onto the centre and you
are back to the blob. At that size a flower has to be drawn *on* the lattice
rather than sampled onto it: one centre voxel and four cardinal petals. That is
the smallest thing that still reads as a flower, and it is the reason 5 cm can
carry a daisy instead of only a sunflower.

**Azimuths are stratified, not random.** Independent draws clump — that is what
random looks like — and on a plant with eleven stems the clumping *is* the
silhouette: every bloom bunched on one side with a bald gap opposite. One stem
per even slice, jittered, fixes it without making a fan.

**The root crown is never smaller than the root spread.** Without a crown each
stem is its own connected component: botanically true, and wrong for an asset
that gets stamped into a world and dug out of it. Left as an independent slider
it silently failed on any wide stand — a reed clump rooting over 18 cm with a
5 cm crown came out 39% detached, which reads as a generator bug and is a unit
mismatch between two sliders that were never tied together.

## Species

### Trees

    temperate-oak       large broadleaf, the baseline, surface roots
    temperate-sapling   small young broadleaf
    river-broadleaf     dense drooping riverbank tree
    birch               slender, pale bark, narrow crown
    cherry-blossom      small flowering, blossom instead of leaf
    hawthorn-scrub      low gnarled hedgerow scrub
    tundra-pine         boreal conifer, tiered (whorl model)
    jungle-emergent     30 m rainforest giant, surface roots
    coast-palm          bare leaning trunk, arcing fronds (frond model)
    weeping-willow      trailing strand curtain (strand pass)
    columnar-cypress    narrow evergreen column (whorl model)
    savanna-acacia      flat umbrella crown
    baobab              enormous trunk, small sparse crown, surface roots
    desert-dead         standing deadwood, no foliage
    field-elm           lone spreading hedgerow tree, authored FOR grassland
    alpine-krummholz    treeline conifer beaten flat by wind and snow

### Bushes

    bramble-thicket     dense low thicket
    juniper-scrub       cold-country scrub, wind-shaped, ground-hugging
    desert-shrub        sparse woody scrub, mostly bare stems
    coastal-scrub       salt-tolerant, wind-flattened, for the beach band

### Rocks

    alpine-scree        0.65 m  small angular fragments for talus slopes
    river-cobble         0.9 m  rounded waterworn stone, clusters near water
    granite-boulder      2.4 m  weathered, part-buried, moderately faceted
    standing-stone       3.2 m  tall fractured monolith
    limestone-slab       3.4 m  flat bedded slab, part sunk
    mossy-forest-boulder 4.5 m  rounded, heavily eroded, sits deep in forest soil
    glacial-erratic      5.0 m  rounded granite left on open ground by ice
    desert-mesa-block    5.5 m  wind-carved sandstone, wide and flat-topped
    cliff-fall-block     6.5 m  sharply fractured, barely eroded, below cliffs
    summit-tor           9.0 m  stack of weathered blocks on high open ground

The five above 4 m are a different job from the five below. A 3 m slab is
scenery you walk past; a 5-9 m boulder is scenery you walk *around* — it blocks
a line, it casts a shadow you stand in, it is a landmark. That is why they are
their own species rather than the granite boulder's size slider dragged right,
and why they are authored rare: `summit-tor` sits at 5% abundance and 120 m
spacing.

### Grass  (5 cm)

    meadow-grass        the baseline tuft, MAT_GRASS
    dry-tussock         coarse pale bunchgrass, stands rather than lies over
    alpine-sedge        short hardy cushion for above the treeline
    jungle-groundcover  broad-bladed rainforest floor cover

### Reeds  (5 cm)

    water-reed          tall stems with seed spikes, clusters near water
    bulrush             shorter and thicker, fat dark heads, reads at distance
    pampas-plume        feathery plume heads on dry open ground

### Flowers  (5 cm)

    meadow-daisy             the baseline; leaf stems and bloom stems together
    alpine-cushion-flower    tiny high-altitude cushion, almost all bloom
    desert-bloom             sparse dry-country flower, wide gaps between plants
    coastal-thrift           low salt-tolerant cushion for the beach band
    jungle-understory-flower taller stems under a few big bright heads

Grass covers all eight plantable biomes. Reeds are absent from desert, taiga and
tundra on purpose. `coastal-thrift` and `jungle-understory-flower` exist because
the coverage tab showed beach and rainforest with no flower at all.

`field-elm` and `alpine-krummholz` exist because the biome coverage tab said so:
grassland and tundra/alpine had species *borrowed* into them and none authored
*for* them. That is the tab doing the job it was built for.

Proportions for birch, cherry, hawthorn, palm and baobab were taken from the
tree scripts in [vengi](https://github.com/vengi-voxel/vengi) (MIT), expressed
in our own parameter vocabulary. The `hanging` crown shape is after their
`tree_domehanging`. No code was copied.

## Shape check

`tools/shapecheck.py` renders an asset with every solid voxel forced to one
bright material. It exists because judging shape off the normal preview is
unreliable in a specific way: rock and bedrock preview colours are dark enough
that on the dark sheet background only the lit top faces show, and a perfectly
round boulder reads as a flat wedge. Twice I "fixed" a shape problem that was a
contrast problem. Colour comes off before shape gets judged.

    python tools/shapecheck.py out.png granite-boulder standing-stone

## How a tree is made

1. **Envelope** (`envelope.py`) — the crown as a volume: sphere, cone,
   umbrella, column, vase, wedge. Scatter growth targets inside it. This is
   where biome variety comes from; a tundra pine and a savanna acacia are two
   different volumes, not two pieces of code.
2. **Skeleton** (`skeleton.py`) — space colonization (Runions et al. 2007).
   Branches grow toward the targets nearest them and consume them on arrival,
   so competition for space produces the branch pattern and branches never grow
   through each other.
3. **Rasterize** (`rasterize.py`, `grid.py`) — draw the skeleton straight into
   a 10 cm grid. Every segment's centreline goes down first as a face-connected
   voxel run, then thickens. Connectivity is guaranteed, not hoped for.
4. **Foliage** (`rasterize.py`) — small ragged clumps on the twigs, jittered in
   size and position.

Before any of that, `spec.realize()` picks *one individual* out of the species:
a bit taller, crown a bit wider, leaning a bit further, facing a random way.
Without it every seed is the same tree with its twigs shuffled — the first
100-seed sheet came out as a hundred oaks all 14.2 m tall, which would make a
seed bank pointless. The `variation` group controls how far individuals stray.

Steps 1–3 work in metres. The only place that knows a voxel is 10 cm is
`grid.VOXEL_M`, which mirrors `vxc::kVoxelSizeMm`.

## Why not generate a mesh and voxelize it

Because at 10 cm a real branch is thinner than one voxel. Meshing first and
voxelizing after (trimesh, cuda_voxelizer) hands the thin-branch decision to
the sampler: twigs vanish or fatten arbitrarily, and the crown can end up not
connected to the trunk. Drawing the skeleton into the grid makes that decision
ours, once, explicitly. Research doc section 4 has the long version.

Voxelization still has a job — the Blender round trip, when a designer edits an
exported tree as a mesh and we bring it back. It is just not the main path.

## Files

    specs/*.json       species specs; the real IP lives here
    forge/spec.py      the parameter table -- single source of truth for
                       validation, the UI's sliders, and what the plain-language
                       box is allowed to touch
    forge/kinds.py     the asset-kind registry; which groups each kind shows
    forge/biomes.py    mirror of vxc::BiomeId, plus which kinds each biome hosts
    forge/pipeline.py  spec + seed -> Asset, plus the per-asset health check
    forge/rock.py      the rock generator; no skeleton involved
    forge/ground.py    grass, reeds and flowers; one tuft generator for all three
    forge/render.py    isometric preview renderer (numpy + Pillow, no engine)
    forge/contact.py   contact sheets
    forge/vox.py       MagicaVoxel .vox export, splitting models over 256 voxels
    forge/vxa.py       VXA1, the compact native format the engine will read
    forge/materials.py material IDs -- slots 0-15 mirror core.h, 16+ are proposed
    forge/web/         the app; hand-written, no framework, no vendored library
    tools/healthpass.py  build every spec at N seeds and print the health checks
    tools/sheet.py       contact sheet of named specs, for eyeballing shape
    tools/shapecheck.py  same, every voxel forced to one bright material
    tools/shots.py       responsive-layout screenshots via headless Chrome
    tools/lattice_ab.py  a plant authored for 2 cm beside the same one authored
                         for 5 cm, drawn at matched physical scale
    tools/retune_5cm.py  one-off that moved the last seven off-lattice assets
    tools/all_to_5cm.py  one-off that put every remaining species on 5 cm
    tools/seed_boulders.py one-off that authored the 4.5-9 m boulders
    tools/seed_kinds.py  one-off that authored the rock/bush/gap species
    tools/seed_ground.py one-off that authored the ground-cover species

## Health checks

Every generated asset is measured, and `batch` reports how many were flagged.
The branch-structure checks are skipped for kinds that have no branches — a
column of zeroes pretending to mean something is worse than no column:

- **wood connected** — the trunk and branches must be one face-connected piece.
  A branch joined only at a corner is a branch that falls off. This must be
  100%; the selftest fails if it is not.
- **attached** — how much of the tree touches anything, corners included.
  Foliage is deliberately speckled, so it is checked more loosely than wood.
- **ground contact** — a tree with nothing on the bottom slab would float.
Size is deliberately **not** among them. Exceeding 256 voxels on an axis used to
be flagged, because that is the per-model limit in `.vox` and the export has to
split. At the 5 cm lattice most trees exceed it, so the flag fired on ten of
forty-two species — which made **Keep all clean** skip every large tree, and a
check that fires on the normal case only teaches you to ignore checks. The
writer splits, the selftest verifies the round trip, and the model count is
reported in the detail stats instead.

## Adding a species

Copy a spec, set its `kind`, change it, run `check`, then `batch` it and look at
the sheet. `python -m forge.cli schema` prints every parameter with its range
and a one-line explanation.

Before calling a change done, run both:

    python tools/healthpass.py 1 2 3     # every species, three seeds, flagged
    python -m forge.cli selftest         # determinism, connectivity, round trips
