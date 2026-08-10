# asset-forge

Cubic-voxel environment assets for voxelsim. Trees first; rocks, bushes, reeds
and grass use the same pipeline later with a different skeleton step.

Why it is built the way it is: `docs/tree-asset-generator-research.md`.
What it is for and where it goes: `docs/tree-asset-generator-plan.md`.

## The idea in one paragraph

A species is a JSON file. Sliders write it, plain-language requests patch it,
batch generation reads it, a seed picks one individual out of it, the library
stores it. A tree is therefore never a blob of voxels we have to keep — it is
`(spec, seed)`, a few hundred bytes that regenerate byte-for-byte the same
voxels every time. That is what makes "hundreds of variants" cheap.

## Status

Phases 1 and 2 work: the generator core and the app. Twelve species.
Phase 0 (making the engine able to hold a tree at all) is deferred until the
editor box is free and is tracked in the plan — **nothing here can be stamped
into the world yet**, because the tree materials do not exist in
`vxc::Material` and the streaming layer still skips the chunks a crown would
occupy.

## Run the app

    python -m forge.cli serve

Opens <http://127.0.0.1:8731/> in your browser. Left panel is every parameter
as a slider, grouped; the middle is a gallery of variants; click one for a
large render, its measurements and **Keep to library**.

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
  measurements. The Library tab lists everything kept, with download links.
- **Save spec** writes the current sliders back to `specs/<name>.json`. Change
  the species name first to fork a new species instead of overwriting.
- **Library** entries each carry **Inspect in 3D**, **More like this** (loads
  that tree's approved spec and generates a fresh block of seeds from it), and
  **Delete** (removes the entry's files from disk, with a confirm).
- `#seed=N` in the URL opens that variant directly, so a particular tree can be
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

- **Gallery tiles** always build at 10 cm (`ASSET_FORGE_PREVIEW_CM`). The
  skeleton is resolution-independent, so a 10 cm preview and a 2 cm export are
  the same tree sampled differently — and no 260-pixel thumbnail can show 2 cm
  detail. A 2 cm species' gallery takes 3.5 s, not 165 s.
- **The 3D viewer** shows the finest size that fits its instance budget
  (`ASSET_FORGE_MAX_VIEWER_VOXELS`, default 2.5M). Small species show at their
  authored size; a 2 cm oak shows at 5 cm and the caption says
  *"shown at 5 cm (exports at 2 cm)"*.
- **Exports** (`.vox`, `.vxa`, Keep to library) always use the authored size.

`pipeline.build` refuses a grid over `ASSET_FORGE_MAX_GRID_MB` (default 3 GB)
before allocating, with the dimensions and the way out, rather than thrashing
the machine. The 28 m emergent at 2 cm needs 1.8 GB and 42 s — it works, but it
is the ceiling of what dense storage handles. Brick-backed storage would cut
that roughly fivefold and is the next step if 2 cm becomes the default for the
largest species.

From the command line: `python -m forge.cli gen specs/tundra-pine.json --res 2`.

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

## Species

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

Proportions for birch, cherry, hawthorn, palm and baobab were taken from the
tree scripts in [vengi](https://github.com/vengi-voxel/vengi) (MIT), expressed
in our own parameter vocabulary. The `hanging` crown shape is after their
`tree_domehanging`. No code was copied.

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

    specs/*.json      species specs; the real IP lives here
    forge/spec.py     the parameter table -- single source of truth for
                      validation, the UI's sliders, and what a language model
                      is allowed to touch
    forge/pipeline.py spec + seed -> Tree, plus the per-asset health check
    forge/render.py   isometric preview renderer (numpy + Pillow, no engine)
    forge/contact.py  contact sheets
    forge/vox.py      MagicaVoxel .vox export, splitting models over 256 voxels
    forge/vxa.py      VXA1, the compact native format the engine will read
    forge/materials.py material IDs -- slots 0-15 mirror core.h, 16+ are proposed

## Health checks

Every generated tree is measured, and `batch` reports how many were flagged:

- **wood connected** — the trunk and branches must be one face-connected piece.
  A branch joined only at a corner is a branch that falls off. This must be
  100%; the selftest fails if it is not.
- **attached** — how much of the tree touches anything, corners included.
  Foliage is deliberately speckled, so it is checked more loosely than wood.
- **ground contact** — a tree with nothing on the bottom slab would float.
- **size** — anything over 256 voxels on an axis is flagged, because that is
  the per-model limit in `.vox` and the export has to split it.

## Adding a species

Copy a spec, change it, run `check`, then `batch` it and look at the sheet.
`python -m forge.cli schema` prints every parameter with its range and a
one-line explanation.
