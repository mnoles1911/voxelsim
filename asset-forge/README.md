# asset-forge

Cubic-voxel environment assets for voxelsim: trees, bushes and rocks today,
grass, reeds and flowers on the same pipeline next.

Why it is built the way it is: `docs/tree-asset-generator-research.md`.
What it is for and where it goes: `docs/tree-asset-generator-plan.md`.

## The idea in one paragraph

A species is a JSON file. Sliders write it, plain-language requests patch it,
batch generation reads it, a seed picks one individual out of it, the library
stores it. An asset is therefore never a blob of voxels we have to keep — it is
`(spec, seed)`, a few hundred bytes that regenerate byte-for-byte the same
voxels every time. That is what makes "hundreds of variants" cheap.

## Status

Phases 1 and 2 work: the generator core and the app. 25 species across three
asset kinds. Phase 0 (making the engine able to hold a tree at all) is deferred
until the editor box is free and is tracked in the plan — **no tree or bush here
can be stamped into the world yet**, because the leaf and bark materials do not
exist in `vxc::Material` and the streaming layer still skips the chunks a crown
would occupy.

Rocks are the exception and worth stating plainly: a boulder is made of
`MAT_ROCK`, `MAT_GRAVEL` and `MAT_SAND`, which the terrain is already made of.
Rocks need no material append at all. They still need the streaming bound to
accept solids above the surface, but that is one blocker instead of two.

## Asset kinds

The app opens on a **section** per kind — Trees, Bushes, Rocks, and three more
declared but not yet generating. The section scopes everything below it: which
parameters exist, which species the dropdown lists, which library entries show,
and which biomes the coverage tab reports on. A rock has no trunk, crown, growth
model or foliage, and the rock section does not show those sliders greyed out —
it does not show them at all.

`forge/kinds.py` is the whole registry. Adding a kind is a generator, a
parameter group and one entry there; it is not a second application.

| Kind | Generator | Notes |
|---|---|---|
| Trees | skeleton (`colonize` / `whorl` / `frond`) | 16 species |
| Bushes | same skeleton, authored short with branches to the ground | 4 species |
| Rocks | no skeleton at all — see **Rocks** below | 5 species |
| Grass, Reeds, Flowers | — | declared, greyed out, no generator yet |

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

Each species carries its own **Voxel size** (the `resolution_cm` slider):
10, 5, 2.5, 2 or 1 cm. The engine's terrain is 10 cm, which is the default; at
2 cm a real twig is several voxels across instead of being rounded up to the
one-voxel minimum, which is a visible gain on foliage and thin branches.

**The cost is cubic** — 2 cm is 125× the voxels of 10 cm for the same tree.
Measured on the species set:

    temperate-sapling   10 cm      8,026 vox   0.1 s        2 cm    981,091 vox   0.5 s
    tundra-pine         10 cm     25,406 vox   0.2 s        2 cm  3,107,527 vox   1.4 s
    temperate-oak       10 cm    108,556 vox   0.4 s        2 cm 13,378,565 vox   5.4 s
    jungle-emergent     10 cm    353,000 vox   0.6 s        2 cm 54,007,589 vox  42.3 s

So the app keeps three resolutions separate, and says which one you are looking
at rather than leaving it ambiguous:

- **Gallery tiles** build at the finest size whose grid fits a cell budget
  (`ASSET_FORGE_PREVIEW_CELLS`, default 6M). A 14 m oak lands on 10 cm; a 1.5 m
  bramble lands on 2 cm. The skeleton is resolution-independent, so a coarse
  preview and a fine export are the same asset sampled differently.

  This used to be a flat 10 cm floor, which was right for trees and badly wrong
  for everything small — a 1 m bush at 10 cm is ten voxels tall, so every shrub
  in the gallery previewed as an unreadable smudge no matter how it was
  authored. Cost is cells, so budgeting cells gets both ends right from one
  rule.
- **The 3D viewer** shows the finest size that fits its instance budget
  (`ASSET_FORGE_MAX_VIEWER_VOXELS`, default 2.5M; a touch client asks for 400k).
  Small species show at their authored size; a 2 cm oak shows at 5 cm and the
  caption says *"shown at 5 cm (exports at 2 cm)"*.
- **Exports** (`.vox`, `.vxa`, Keep to library) always use the authored size.

Small assets are authored fine, because that is what 2 cm is for: the bushes and
alpine scree are 2 cm, river cobble 2.5 cm, saplings and hawthorn 5 cm, the
large trees 10 cm.

### What 2 cm exposed

Two bugs sat harmlessly in the foliage code for as long as everything was 10 cm,
and both showed up as thousands of loose voxels the moment a clump was fifty
voxels across instead of two:

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
the machine. The 28 m emergent at 2 cm needs 1.8 GB and 42 s — it works, but it
is the ceiling of what dense storage handles. Brick-backed storage would cut
that roughly fivefold and is the next step if 2 cm becomes the default for the
largest species.

From the command line: `python -m forge.cli gen specs/tundra-pine.json --res 2`.

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
corrects, at most three times, on the same seed — so the slider is honest to
within a voxel or two.

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

    granite-boulder     weathered, part-buried, moderately faceted
    river-cobble        rounded waterworn stone, clusters near water
    limestone-slab      flat bedded slab, part sunk
    standing-stone      tall fractured monolith
    alpine-scree        small angular fragments for talus slopes

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
    tools/seed_kinds.py  one-off that authored the rock/bush/gap species

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
- **size** — anything over 256 voxels on an axis is flagged, because that is
  the per-model limit in `.vox` and the export has to split it.

## Adding a species

Copy a spec, set its `kind`, change it, run `check`, then `batch` it and look at
the sheet. `python -m forge.cli schema` prints every parameter with its range
and a one-line explanation.

Before calling a change done, run both:

    python tools/healthpass.py 1 2 3     # every species, three seeds, flagged
    python -m forge.cli selftest         # determinism, connectivity, round trips
