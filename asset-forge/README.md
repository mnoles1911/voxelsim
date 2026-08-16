# asset-forge

Cubic-voxel environment assets for voxelsim: trees, bushes, rocks, grass, reeds,
flowers and fish.

Why it is built the way it is: `docs/tree-asset-generator-research.md`.
What it is for and where it goes: `docs/tree-asset-generator-plan.md`.
Why the fish are shaped the way they are: `docs/fish-shape-research.md`.
What changes at 25 metres: `docs/marine-megafauna-research.md`.
Why a marking can have a shape, and which sex you get: `docs/marine-marking-research.md`.
Which birds have a sex worth drawing, and which twelve do not: `docs/bird-dimorphism-research.md`.
What fish colour needs from the engine: `docs/fish-colour-proposal.md`.
How the heroes hid a defect behind a single seed: `docs/hero-sequoia-wood-detachment.md`.
What grows and what lies on the bottom in salt and fresh water, and the three
decisions behind it: `docs/aquatic-species.md`.

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

The app opens on a **section** per kind — Trees, Bushes, Rocks, Grass, Reeds,
Flowers and Fish. The section scopes everything below it: which
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
| Fish | lofted body + thin fin plates — see **Fish** | 17 | landed |
| Whales & dolphins | the same generator; horizontal fluke, flippers, blowhole | 7 | landed |
| Birds | jointed layout: body, neck, head, bill, tail, wings — see **Birds** | 20 | yes (eleven plumage colours) |

**Fish are the first animal here, and the first asset that does not stand on
the ground.** They are placed IN water, they face a direction, and they are not
meant to persist — see **Fish** below.

**Birds are the first JOINTED asset.** Everything before them is one shape: a
tree is a skeleton, a rock is a carved lump, a fish is a solid whose
cross-section changes along a straight axis. A bird is six parts at angles to
each other, and the angles are most of what tells one apart from another — see
**Birds** below. They are also the first kind that can go in **Ocean** and on
**Bare rock**, the two biomes that hosted only fish and only boulders.

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

### Weathering: one mechanism, two families of rock

The weathering pass is driven by the **sign of the local curvature**, which is
the mechanism the geology and graphics literature uses rather than a hand-rolled
proxy (Beardall et al., *Goblins by Spheroidal Weathering*, EG Natural Phenomena
2007; Jones et al., *Directable Weathering of Concave Rock Using Curvature
Estimation*). It is what lets one generator produce two entirely different
kinds of stone:

- **Spheroidal weathering** attacks CONVEX surfaces fastest. A corner is exposed
  on more sides than a flat face, so it goes first: blocky stone rounds off and
  granite sheds curved shells. It is why a boulder is a boulder.
- **Cavernous weathering** attacks CONCAVE surfaces fastest. Once a pit exists
  it traps moisture and salt, so it deepens faster than the rock around it, and
  the runaway pitting carves tafoni, honeycomb sandstone and hollow-sided
  goblins.

Both fall out of one number: the solid fraction inside a small ball centred on
each voxel — half on a flat face, less on a protruding corner, more inside a
hollow. `0.5 - fraction` is a signed curvature, and `rock.cavernous` picks which
end of it drives the erosion. That ball is the "bubble" of the 2007 paper, and
it is cheap on a voxel grid, which is why this is the one weathering model worth
having here.

Two details it will not work without. It has to be **iterative** — a pit only
runs away if the next pass sees the pit the last one made — and the noise must
be **the same across passes**, so the same weak patches are attacked repeatedly.
Fresh noise per pass averages the feedback away and gives even pitting instead
of cavities.

### Bedding: differential erosion

`rock.bedding` lays alternating hard and soft layers through the stone as a
per-voxel durability field, dipped and noise-roughened. Sedimentary rock is
deposited in beds of differing hardness, and that one fact is behind most rock
shapes people recognise: weather a uniform block and you get a rounded lump
whatever else you do; weather a layered one and the soft beds retreat while the
hard ones stand proud. Banded cliffs, undercut pedestals, mushroom rocks and the
capstone on a hoodoo are all this.

It is the single biggest variety lever in the rock generator, because it changes
the *class* of rock rather than its proportions.

### Fracture: joint sets, blocks and columns

**Rock does not fracture in random directions.** It fractures along a small
number of **joint sets** — typically a bedding plane plus two near-vertical sets
roughly at right angles — and every face in an outcrop shares them. That
correlation between faces is the entire signal: it is what makes granite read as
quarried rather than merely lumpy, and drawing each facet normal independently
could never produce it. `rock.joint_sets` builds one orthogonal frame per rock
and draws every cut from it.

**`rock.block_relief_m` opens the joints.** Once the joint frame exists it also
defines a lattice of blocks, and letting the planes stand open turns one stone
into an outcrop of separate ones. This is the voxel form of what the
implicit-blocks literature (Paris et al., *Modeling Rocky Scenery using Implicit
Blocks*, CGI 2020) does with signed distance fields. The visible **gap** is the
whole effect — a continuous mass with faces drawn on it still reads as one rock.

**`rock.columns` gives basalt.** Cooling lava contracts into a polygonal crack
network that propagates downward. The network is a 2D Voronoi tessellation
extruded along the column axis, found with the standard F2−F1 test: a voxel is
on a cell boundary when its two nearest seeds are nearly equidistant. Seeds sit
on a jittered lattice rather than uniformly at random, because the literature
puts real column networks between a random and a centroidal tessellation —
purely random seeds clump and give a few huge columns beside slivers. Columns
get their own top heights, because a colonnade sawn flat reads as an extruded
shape rather than as stone.

**`rock.exfoliate` peels shells.** Granite domes release pressure as the load
above them erodes away and split into sheets *parallel to the surface*, which
spall off. Depth below the surface is a distance transform and a coherent noise
field decides how many whole shells each patch has lost; peeling in whole shells
rather than continuously is what leaves the stepped edge. The ordinary
weathering pass cannot produce this at any setting, because it has no notion of
depth.

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

## Fish

The first environmental animal, and the first asset here that is not rooted to
the ground (`forge/fish.py`). A fish is three things stuck together, and they
fail in completely different ways, so the code keeps them apart:

1. **A lofted body** — a spine with a depth and a width at every station along
   it, filled as a superellipse cross-section.
2. **Thin plates** — the caudal, dorsal, anal, pectoral and pelvic fins, plus an
   adipose fin and barbels where the species has them.
3. **A colour scheme** — countershading, one marking, and an eye.

### The budget decides the design

A fish here is twenty to forty voxels long: about 600 solid voxels, of which
about 120 are on the silhouette. That cannot carry anatomy, so it is spent on
the three things the eye actually uses at a glance — the OUTLINE, the COLOUR
SCHEME and the EYE. Everything else was measured against that budget and most of
it lost. `docs/fish-shape-research.md` has the full list; the short version of
the rejections is that the superformula is pixel-identical to a plain ellipse at
a 4x8 cross-section, elliptic Fourier descriptors put the fin-encoding harmonics
below the quantisation noise floor, and a Turing pattern with a real fish's
wavelength resolves as salt and pepper on a body twelve voxels deep.

### Authored at 1 cm, not the 5 cm everything else uses

Cost is not the constraint. A whole fish is 150-4,600 voxels against 75,000 for
a temperate oak, so a shoal of forty is under a tenth of one tree. What decides
it is whether the FEATURES still exist, measured with
`tools/fishprobe.py --lattice`:

    brown-trout        1 cm   41 voxels long   fork depth 5   eye present
                       2 cm   21               fork depth 2   eye present
                       5 cm    8               fork depth 0   eye gone
    clown-anemonefish  1 cm   18               --             eye 2 voxels
                       2 cm    9               --             eye gone

**At 5 cm the tail fork and the eye are gone on every species**, and between
them those two are most of what makes a small object read as an animal rather
than as a lozenge. 1 cm nests 10:1 in the terrain lattice and 5:1 in the asset
lattice, both whole numbers, so a fish placed on a fine-lattice coordinate lands
exactly on it.

What 1 cm does not buy is a life-sized small fish. A 10 cm anemonefish is ten
voxels, and three bars need at least twelve. `clown-anemonefish` and
`pale-minnow` are therefore authored at the large end of their real size range
and their spec notes say so. A 5 mm tier would fix it and would be a THIRD asset
lattice; that is an owner decision, not a generator decision.

### Three things that were got wrong first

**A fin drawn at its true position falls off.** Every fin is a plate one voxel
thick standing on a curved body, so placing it AT the surface leaves it touching
at a corner, or not at all, depending on which side of a rounding decision the
surface landed. Every fin is now drawn starting ONE VOXEL INSIDE the body. The
pectoral goes further and starts at the body's own axis, because the widest
point of a fish is exactly where it is thinnest vertically, and a fin voxel
placed at the surface there often had no body voxel beside it.

**A body drawn only by its cross-section can vanish.** The snout and the caudal
peduncle are the thinnest parts of a fish — an eel's peduncle is 0.2 voxels at
this size — and a cross-section that thin contains no cell centre at all, so the
nose and the tail wrist dropped out and the asset shipped in three pieces. The
body axis is stamped as a solid one-voxel run FIRST and the cross-sections are
added to it, which is the same rule `grid.capsule` uses for a twig.

**A fork reaches the full span at the trailing edge**, which is geometrically
what a fork IS and which left each lobe exactly one voxel wide: two whiskers
rather than a tail. Lobes are now held to a quarter of the span, or 1.2 voxels,
whichever is more.

### Colour carries the species, not shape

Two fish of the same outline in different schemes read as two species; two fish
of different outlines in the same scheme read as one species at two sizes. That
is a claim about twenty-voxel fish specifically, and it falls straight out of
the budget: the outline has about 120 voxels to work with and the flank has
about 400. `golden-carp` and `river-perch` have similar outlines and nobody
confuses them.

Three layers, in order. **Countershading** — dark back over a pale belly, which
nearly every fish in open water wears and which is the only thing that gives a
voxel fish a top and a bottom at all. Then **one marking**. Then **the eye**.

One marking and never two: a flank twelve voxels deep cannot hold a stripe and
bars without them reading as noise, and in nature they are mutually exclusive
anyway. The five are `stripe` (horizontal, the open-water schooling mark),
`bars` (vertical, the weed-bed and reef mark), `spots`, `mottle` and `saddle`.

**The eye is two voxels a side and worth more than any other two in the asset.**
A voxel animal without one reads as an object; with one it reads as facing
somewhere. The pale voxel in front of it is not a highlight, it is a contrast
partner — a dark eye on an olive or brown head disappears entirely without one.

### A marking can be an EDGE, not just ink

The five markings above are all ink laid ON a colour field, and they are drawn
over a countershading whose two boundaries are **level lines** running the
length of the animal. The three most recognisable colour schemes in the sea are
none of those things: they are the *boundary itself* having a shape.
`fish.field_curve` bends it.

- **`cape`** — the dark back reaches down onto the flank around the dorsal fin
  and lifts again behind it. Every dolphin wears one.
- **`flame`** — the pale belly throws a blaze up the flank behind the middle of
  the animal. It is why an orca reads as two white shapes from the side and not
  one.
- **`hourglass`** — both, at the same place, so the dark and the white meet and
  pinch the flank out between them. That is the common dolphin's criss-cross.

**The third one is the argument for doing it this way.** An hourglass is not a
third mechanism — it is the first two meeting, and it falls out for free.
Perrin's 1972 account of *Delphinus* says exactly that: the four colours of a
common dolphin are the overlap of **two** shapes, not four regions, so the
forward half of the pinched flank is a warm chestnut and the half behind it
grey. Drawn
instead as an hourglass-shaped MARKING it would have been a hand-drawn X that
knew nothing about where the countershading was, and moving the dark-back
slider would have left the X floating clear of the cape it is cut out of.

**Two voxels or it is not a curve, and that excludes the small fish by
measurement.** `python tools/fishprobe.py --marks` prints the minimum setting
each species needs: an orca needs 0.07 of its body depth, a bottlenose dolphin
0.22, and a herring **0.67** — two thirds of its own flank, at which point the
"curve" is the countershading. The bottlenose is the shallowest animal in the
library that can carry one at all, and it gets exactly two voxels.

`common-dolphin` is authored at **2 cm** rather than the 5 cm the other
dolphins use, and it is the first time a COLOUR feature has chosen a lattice
here. At 5 cm its cape dips one voxel; at 2 cm it dips four and the blaze rises
seven.

### The hammerhead's head

`fish.head_width` is how far the head sticks out sideways, tip to tip, as a
fraction of body length — 0 on everything except one group.

**It is the only thing in the group the body loft could not say.** Every other
width here follows the depth: one slider decides how the whole animal flattens
from nose to tail, so a station that is shallow is narrow. A hammerhead is the
exact counter-example — its head is the shallowest part of the animal and by a
long way the widest — and until this parameter existed, typing `hammerhead`
gave a shark with hammerhead proportions and an ordinary head.

It widens the loft itself rather than adding a plate, so the head is the same
solid as the body and connectivity is a fact rather than a hope. Two things
come out of that for free and are measured because they were free: the **eye
lands on the wingtip**, which is where a hammerhead's eyes are, and the head
carries no join to come apart. Measured on `scalloped-hammerhead`
(`--head`): span **27.5% of total length** against a published 25–32%, chord
0.32 of span against Compagno's "less than half", thickness 0.08 of span
against the one measured animal's 0.094.

A cephalofoil is a horizontal plate, exactly like a fluke — so the 8-degree
fish camera sees it edge-on and `render.camera_for` sends a wide-headed fish to
the 30-degree camera the whales use. Same exception a flying bird needs, same
geometric reason.

### Male and female

`fish.sex` is `unsexed`, `female` or `male`, and three ratios say how far apart
the sexes are: body length, dorsal fin height and flipper reach. **The authored
numbers are the species average and each ratio is split as a square root either
way**, so male ÷ female is exactly the ratio and neither sex is the default.
The obvious alternative — the spec is the female and the male is scaled up —
would have made every unsexed spec in the library silently female.

Forcing that question to be answered out loud found that three specs could not
answer it. `orca`'s dorsal fin was 27% of its body length when the published
bull is 22–26% — taller than any measured male — while its **flippers were a
female's at the same time**; `whale-shark`'s 9 m and `sperm-whale`'s 16 m are
both male figures against females of 14.5 m and 11 m. Those three were
re-authored onto the average. Everything else keeps its authored size: the rule
was *re-author only where leaving the number alone would put both sexes outside
their published ranges*.

**Eleven of the twenty-three species carry a sex worth drawing and twelve do
not, and the twelve say so.** A river perch's females are 6% longer, which is
1.6 voxels; a herring's difference is 0.4%; a blue tang has none published at
all. `python tools/fishprobe.py --sex` prints the movement in voxels per
species and flags any species that claims a ratio and does not get it.

**Sex reseeds, and that is the opposite of what a pose does.** A raven perched
and the same raven flying are one animal, so `bird.pose` is excluded from the
seeding hash. A male orca and a female orca are two animals — there is no
individual that is "the same whale, but female" — so seed 7 male and seed 7
female are two different whales. `--sex` checks the hashes differ rather than
letting it be an accident of where the field was put.

### The materials

ADR-0008 gives every voxel face one flat colour from `vxc::kMaterialPalette`,
and that palette was terrain-oriented: no orange, no silver, no reef blue. Ten
`MAT_SKIN_*` entries were proposed and **were approved and appended to the
engine on 2026-08-13** — ids 26-35, `kMaterialCount` 26 to 36. They are in
`forge/materials.py` — dark, pale, silver, olive, brown, orange, yellow, red,
blue, green — named for what they look like rather than for what wears them,
because fish will not be the last animal. `docs/fish-colour-proposal.md` has the
exact table, the files the append touches and what each one costs. The birds
proved that list one file short: it does not name
`ue-project/Tools/terrain_palette.py`, which refuses to generate until every new
material has a `BIOME_TINT` decision and which stopped the fish append's
generator dead. See `docs/bird-colour-proposal.md`.

### Detail entities

The `detail` group is authored and **read by no code**, deliberately, in exactly
the way the `placement` group has been since this library began. Spawning fish
into water bodies is worldgen's job, not the generator's. What these rows are is
the specification a spawner will be written against, kept in the same file as
the shape so the two cannot drift apart: which water bodies a species uses, how
deep it holds, how big a shoal is, how many per hundred square metres of water
surface — and the thing that makes a shoal of forty affordable at all,
`detail.entity_class`, which says that nothing about an individual is saved and
that it is deleted shortly after the player leaves.

### Species

    brown-trout          0.30 m  fusiform, olive-brown, spotted, adipose fin
    river-perch          0.22 m  deep olive body, five dark bars, spiny dorsal
    pale-minnow          0.20 m  plain silver; the test of how little will do
    river-eel            0.70 m  anguilliform, ridge fin, no fork, no pelvic
    northern-pike        0.75 m  sagittiform, mass carried aft, pale saddles
    mud-catfish          0.38 m  wider than deep, barbels, brown mottle
    golden-carp          0.40 m  deep orange body, two barbels
    clown-anemonefish    0.18 m  three pale bars on orange, rounded tail
    reef-tang            0.18 m  a disc on edge: blue body, yellow fins
    shoal-herring        0.20 m  countershading and nothing else, deep fork

### Keyword input

The plain-language box takes species names — *trout*, *pike*, *eel*, *carp*,
*clownfish*, *tang*, *tuna*, *pufferfish*, *catfish*, *flounder*, *barracuda*
and about a dozen more — and each one replaces the body proportions, the fin
arrangement and the markings TOGETHER, because those three are what a species is
at this size and setting one without the others gives a trout wearing pike
colours. It also takes descriptions: *deeper bodied*, *streamlined*, *lunate
tail*, *barred*, *bolder markings*, *bigger eye*.

It is local, like the rest of `forge/language.py` — no network, no model, no
key. The numbers are the published medians per shape class, typed out by hand.
FishBase carries a body-shape class for 36,125 species and would have given a
far larger vocabulary for free; it is also explicitly licensed for
non-commercial use, and the one public dataset that carries body WIDTH
contradicts itself about its own licence. Neither belongs inside a game.

### Proving the sliders do something

`tools/fishprobe.py` sweeps every fish parameter across its authored range,
measures a number a person could check off a render, and prints **DEAD** when it
does not move. It exists because the silent no-op is this project's signature
failure, and it earned its place on its first run by finding one — and then by
finding that three of its own measurements were wrong: `dorsal_len` makes a fin
longer rather than heavier, `pattern_count` makes more bars rather than more
ink, and `pattern_pos` moves a stripe without changing how much of the fish it
covers. All three had been measured by voxel count and all three reported a
working slider as dead.

    python tools/fishprobe.py              # every parameter, 4 seeds averaged
    python tools/fishprobe.py --read       # silhouette, value contrast, blur
    python tools/fishprobe.py --lattice    # 1 cm against 2 cm and 5 cm
    python tools/fishprobe.py --marks      # shaped colour boundaries, in voxels
    python tools/fishprobe.py --head       # head span: the cephalofoil
    python tools/fishprobe.py --sex        # male against female, in voxels
    python tools/fishprobe.py --ab         # the three A/B render sheets

It averages over seeds because changing any parameter changes the spec hash, and
the hash is mixed into the seed — so a one-seed A/B is not the same fish twice,
it is two different fish.

The `--read` pass is the one to run before approving a species. It flags a fish
under 18 voxels long, a marking covering under 8% of the body, and — the one
that matters most — a marking whose **value contrast** against the flank is
under 1.5. The eye carries brightness at roughly four times the spatial detail
it carries colour, so a marking that differs from the flank only in HUE blurs
away about four times sooner: it can be perfectly present in the voxels and
invisible in the water.

## Birds

`forge/bird.py`. Twenty species, authored at **1 cm**, **334 to 28,355
voxels each** — all twenty together are 87,460, against 1,065,343 for one
`temperate-oak`, so a flock of forty song-thrushes costs 1.25% of one tree.
Research and sources in `docs/bird-shape-research.md`; the colour ask in
`docs/bird-colour-proposal.md`.

**A bird is not a fish with wings, and that is the whole reason it is a second
generator.** A fish is one solid whose cross-section changes along a straight
axis, so a loft draws it. A bird is a body, a neck, a head, a bill, a tail and
two wings, each at its own angle to the others. A heron and a mallard have
nearly the same body; what separates them at twenty voxels is that the heron's
neck is a quarter of its length, its bill is a spike and its legs are a third of
it again.

So the file is a **layout** followed by six drawing passes. The layout is solved
first, in floating-point voxel coordinates, as a handful of points and radii;
the grid is sized from that layout's own bounding box rather than from a
formula; and each part is drawn starting one voxel INSIDE the part it hangs off.

### What the voxel budget is spent on

Birders identify birds by shape and stance before colour. They have a word for
it — *jizz*, first recorded in a *Manchester Guardian* column in **1921**, which
settles the folk etymology that it is an RAF acronym — and Cornell's own
teaching material enumerates the cues: "the head, the bill, the length of the
wings and the length of the tail", compared **against each other** rather than
against a ruler.

That is exactly what survives here, so it is exactly what the parameters are:

1. **Proportion.** Five shares — bill, head, neck, body, tail — normalised to
   sum to one. This is the strongest thing in the file. A heron's neck is 0.26
   of its length and a starling's is 0.03; a macaw is 0.55 tail and a kingfisher
   is 0.11.
2. **Posture.** One angle. A duck lies flat at 4 degrees, a thrush sits at 34, a
   woodpecker clings to a trunk at 68.
3. **The bill.** Four to sixteen voxels, and it says what the bird eats.
4. **The tail outline.** Seven field-guide shapes out of one function.
5. **The wing** — but only when it is spread.
6. **One mark per region**, on three disjoint regions.

### The pose problem, and what was decided

A perched raven is a dark lozenge and a flying raven is a cross. Those are not
one shape at two rotations: the folded wing is a three-voxel bulge lying along
the flank and the spread wing is a one-voxel plate reaching thirty voxels out,
and **no rotation turns one into the other at this resolution**. One generation
produces one asset, so an asset carries one pose.

Every prior art agrees on the shape of the answer and none of it transfers
directly. Minecraft ships **one** parrot geometry and six animations, with the
wings moved purely by bone rotation from a folded rest pose. Infinigen exposes
one continuous `Extension` scalar from folded to spread. Avian-mesh ships two
JSON files whose faces, kinematic tree and skin weights are byte-identical and
whose **rest vertex positions are not**. All three store one topology and **two
rest configurations**.

**A cubic voxel asset cannot store a rotation delta. The grid IS the rest
configuration.** So the voxel-space equivalent of "one mesh, two rest poses" is
**one spec, two poses**, which is what `bird.pose` is — and it costs one changed
field in a four-kilobyte JSON file, not a second asset library.

The measurement that came out of avian-mesh is the one to remember: spreading
the wings multiplies the **span** by 4.51 and the **body length** by 1.04. Key
the voxel budget to body length, which is pose-invariant. `render.predicted_extent`
does, and `render.camera_for` sends a perched bird to the broadside camera and a
flying one to the isometric, because a flying bird holds its wings along the
broadside camera's own axis and hides the entire planform from it.

`python tools/birdprobe.py --pose` reproduces that here, over all twenty
species: **span multiplies by 2.9 to 10.0 and length by 0.89 to 1.21.** The two
that move most in length are the ones that should — a woodpecker is authored at
68 degrees nose-up and lies down to fly, so its x extent grows by 1.69, and a
swallow is authored nearly level already. Voxel counts roughly double to triple:
a raven is 3,896 perched and 11,728 flying.

**All twenty species are authorable in both poses.** The pose in the spec is
where the species is usually seen, not a restriction, and `birdprobe --pose`
builds all forty and checks each is one piece at 26-connectivity.

### A pose is a posture, not a different bird

This used to be untrue and it was the one thing a spawner had to work around.
`pipeline.rng_for` seeded from `spec.spec_hash`, the pose is part of that hash,
and a different hash is a different random stream — so `common-raven` seed 7
perched and `common-raven` seed 7 flying were **two different ravens**,
different size and different markings. A bird could not land without changing on
the way down.

`rng_for` now seeds from **`spec.seed_hash`**, which is `spec_hash` with
`bird.pose` normalised to its default before hashing. `spec_hash` itself is
unchanged and still identifies the spec everywhere else — the library entry, the
`.vxa` metadata, the preview cache key — because a perched raven and a flying
raven are genuinely two assets.

**Normalised rather than deleted, and that is the whole design.** Every
validated spec carries every parameter, birds included, so *deleting*
`bird.pose` before hashing would change the canonical JSON of a tree, a rock and
a fish too and reseed the whole library for the second time in one day.
Normalising to the default leaves the bytes untouched for anything that never
authored a pose. Measured over all 109 specs: **104 keep the exact seed they had
— every tree, rock, fish and cetacean, and the fifteen perched birds — and the
five species authored `flying` reseed once**, onto the individual their perched
twin already was.

    python tools/birdprobe.py --pose        # 20/20 species, one bird in both poses
    python tools/birdprobe.py --pose-ab     # the render: out/birds/birds-pose-ab.png

The A/B sheet draws each species folded and spread at one seed **with individual
variation ON** — the previous version pinned it to 0, which is a workaround for
this very defect and hid it — and a third cell per row at the next seed as the
control. Columns 1 and 2 must be the same animal; column 3 must not be.

`--pose` checks it three ways and each one fails loudly on its own: the seed
hash, the ten varied numbers `bird._params` hands the drawing code, and the bill
in voxels. The bill is the only part the pose touches neither directly nor
through the grid's own mid-plane, which is why the voxel half of the check is
that narrow — the two poses genuinely do not rasterise on the same lattice, and
saying so is better than a tolerance.

### Male and female

Sourced species by species in `docs/bird-dimorphism-research.md`, with the
rejections carrying the numbers that decided them.

**In fish, sexual difference is size. In birds it is colour**, and that one
sentence is the whole design. `fish.sex` got twenty-three species out of three
ratios and a square root, because what separates a bull orca from a cow is the
height of a fin. The largest difference between two animals of one species
anywhere in this library is a mallard drake against a hen — bottle-green head,
white collar, grey body and yellow bill against uniform mottled brown — and
**not one voxel of it is a proportion**. So `bird.sex` drives two mechanisms.

**The size half is the fish rule verbatim.** `bird.sex_length` and
`bird.sex_tail` are male-to-female ratios, applied as `sqrt(r)` and
`1/sqrt(r)`, so male ÷ female is exactly the ratio, the authored number is the
species average and neither sex is the default. Three species carry a length
ratio and one a tail ratio. Unlike the fish, **three of the four are the female
being larger** — reversed sexual size dimorphism is the rule for raptors, and
the golden eagle at 0.93 (wingspans 2.02 m against 2.16 m) is the largest
size difference in the set.

**The colour half cannot be, and says so.** There is no average of a green head
and a brown one, so a dimorphic species is authored as ONE sex,
`bird.sex_plumage` names which, seven `materials.bird_alt_*` rows and three
`bird.sex_alt_*_mark` rows say what the other sex swaps, and `unsexed` draws
the authored one. `same` on any row means "whatever the species carries", so a
great spotted woodpecker — whose entire published difference is that the male
has a crimson nape and the female does not — authors one row and not ten.
**That limitation is printed by the probe on every run**, listing the four
species where `unsexed` is really one of the sexes.

**Eight of the twenty carry a difference and twelve are an honest null** —
1.00, 1.00, `same`, and measured moving no geometry at all. The rejections are
where the work is: a tawny owl's females are 5% longer, which is **1.6 voxels**;
a kestrel's size difference is 1.9; every passerine here needs 8–17% linear
dimorphism before the difference is a voxel and a half, and passerines have
2–4%. A kingfisher's female has an orange lower mandible and **the bill is one
voxel deep**. A starling's sexes differ in bill-base colour, throat hackles and
iris — three voxels, no feather texture, and a two-voxel eye.

**No bird's authored numbers were re-authored.** The aquatic work moved three
specs off male references; the same check run here — apply both square roots and
test each result against that sex's published range — lands every species
inside. But four specs *were* authored at one sex, in their COLOURS, which is
the same trap through a different door and could not be fixed the same way. It
is declared instead.

**Sex reseeds, which is the opposite of what the pose does**, and the section
above is why that needs saying twice. There is no individual that is "the same
mallard, but female", so `bird.sex` is deliberately not in `SEED_INVARIANT` —
and `--sex` checks the hashes differ on all twenty species, including the twelve
that look identical, because on those it is the only thing separating "no
dimorphism" from "the parameter never reached the build".

    python tools/birdprobe.py --sex        # male against female, in voxels
    python tools/birdprobe.py --sex-ab     # out/birds/birds-sex-ab.png

`--sex` reports the movement in voxels, the repainted area **against each
species' own marking-phase noise** (a 314-voxel song thrush moves 6% of its
histogram between two individuals without a single decision differing, because
its speckle quantile keeps the count and not the positions), and two tables that
exist because of holes found while building it. Six of the seven colour slots
are used by one species and two of the ten rows by none, so **every `alt` row is
exercised on a rig** that authors none of them, with a control that it does not
leak to the sex the spec is authored as. And **the other sex's colours are put
through the same contrast floor `--read` uses** — which caught a real defect on
its first run: the hen mallard, left at the drake's yellow bill, measured 1.20
against her buff head. An invisible bill, which is the exact defect ten of these
twenty species shipped once before.

### Wing planforms

Savile's 1957 classification maps almost one-to-one onto groups a player would
name, and it is `bird.wing_shape`:

| Planform | Groups | Tip |
|---|---|---|
| `elliptical` | corvids, gamebirds, woodland songbirds | broad, rounded |
| `pointed` | falcons, swifts, swallows, terns, ducks | swept, tapering |
| `soaring` | gulls, albatrosses | a long narrow plank |
| `slotted` | eagles, buzzards, storks, vultures | separated finger feathers |

**Aspect ratio is a separate slider and that is not redundancy.** The planform
says how the chord is DISTRIBUTED; aspect ratio says how much there is. Measured
over 129 species from 33,610 individual measurements (Alerstam et al. 2007), the
three raptor shapes are falcon **7.9**, accipiter **6.2**, buteo **5.6** — all
three are raptors, all three read differently in the air, and the difference is
one number. A jay is 4.5, the lowest measured, which is why its wing is so short
and round.

**"Soaring" is not one wing.** Hand-wing index separates ocean dynamic soarers
(albatross 60–67) from land thermal soarers (eagle and vulture 27–39) by a
factor of two. A vulture gets its low wing loading from AREA, not from
pointedness, and drawing a golden eagle with an albatross silhouette is a
visible error. That is why `soaring` and `slotted` are two entries.

### Three markings, where a fish gets one

That is not a relaxation of the fish rule; it is the same rule. A fish's stripe
and its bars are drawn on the same twelve-voxel flank, so two of them is noise.
A bird's cap is on its head, its wing bar is on its wing and its streaking is on
its breast, and **those three sets of voxels do not overlap at all**.

The head also gets its own marking colour, and there is evidence for the
asymmetry: CUB-200-2011, the standard expert-annotated bird dataset, gives the
head **eleven** pattern values and gives the breast, back, belly, wing and tail
**four** each. Ornithologists spend nearly three times the vocabulary on the
head. The species that forced it was the great spotted woodpecker, which is
white-panelled on the wing and crimson on the nape; with one shared marking
colour it had to choose.

`bird.upperparts` is ONE hard boundary rather than a gradient. Cuthill et al.
2016 found a sharp countershading transition is optimal under **direct** solar
illumination and provides no advantage at all under diffuse light — and a hard
boundary is also what every field guide draws.

### Colour is pushed past life, and there is a number behind it

Delhey 2015 measured 46,559 reflectance spectra over 555 species: **melanin
accounts for 74% of the plumage area and 7% of the colour gamut, while
structural colour is 7% of the area and 45% of the gamut.** A palette weighted
by area — which is what copying a field guide gives you — is browns and greys.

So the rare colours are deliberately over-weighted. A raven carries its gloss as
a real teal, a pigeon's neck is really lilac, a kingfisher is turquoise rather
than the deep blue it photographs as. **Where a species is pushed, its spec
notes say so**, so nobody later "corrects" it back.

Two independent measurements say colour is worth the material slots. Torralba
found humans need 64×64 in greyscale for what colour does at 32×32 — colour is
roughly a **2× resolution multiplier**. And *Pixel Logic* records that Super
Mario World's Swoopers are bats that read as birds purely because their nose was
coloured orange: **an orange protrusion in the head position converts a bat into
a bird.** That is why `beak_horn` exists and why nine of the twenty species
carry a bright bill.

**Eleven `MAT_PLUME_*` entries were proposed and were approved and appended to
the engine on 2026-08-13** — ids 36–46, `kMaterialCount` 36 to 47. Four are the
neutrals the fish skins have no equivalent for (a fish belly is cream, not
white; the one grey in the skin set is the mirror-flanked silver; the palette
had no dark cool grey and no sandy tan at all), six are saturated hues, and one
is keratin. `docs/bird-colour-proposal.md` has the table, the WCAG contrast
number behind every entry and the five files plus two regenerated artifacts the
append touched.

The twenty species fill 160 material slots between them: **92 are one of the
eleven and 68 are one of the ten fish skins**, which is why the ask was eleven
and not twenty-five. Two entries break the darkens-downward convention on
purpose — `plume_iridescent` has brighter sides than its top, because
structural colour is an angle effect and a flat dark green draws a starling as a
blob, and `plume_white` carries the lowest jitter in the whole engine palette,
because noise on a white bird reads as dirt.

Nothing about a bird is a stand-in any more. `python -m forge.cli selftest`
checks it: **every asset in the library uses only material ids the engine has**,
against the count read from the generated palette. Before the append that check
refused all twenty birds and no other species, which is what
`AssetGrid::materialsWithinEngine` does in C++ and is the reason the check is
worth having on this side too.

### Authored at 1 cm, and four species above life size

`tools/birdprobe.py --lattice` measures the features rather than the cost,
because cost is not the constraint:

| Species | Lattice | Length | Bill | Eye | Tail | Neck | Crest | Voxels |
|---|---|---|---|---|---|---|---|---|
| european-robin | 1 cm | 19 | 2 | **2** | 7 | 1 | 1.0 | 445 |
| | 2 cm | 10 | 3 | 1 | 5 | 1 | 1.0 | 85 |
| | 5 cm | **7** | 3 | 1 | 5 | 0 | 1.0 | 41 |
| eurasian-hoopoe | 1 cm | 26 | 5 | **2** | 9 | 5 | **9.0** | 435 |
| | 5 cm | **9** | 2 | 1 | — | **0** | 2.0 | 35 |
| golden-eagle | 1 cm | 83 | 8 | **2** | 15 | 3 | 5.5 | 28,355 |
| | 5 cm | 17 | **0** | 1 | 4 | 3 | 0.0 | 1,189 |

**At 1 cm every species has a two-voxel eye — a pupil and its contrast partner.
At 5 cm every species has one and no partner**, which is a dark speck on a dark
head, and **the bill is gone entirely on nine of the twenty species**, the
eagle's included. At 2 cm a robin is ten voxels long, below Minecraft's own
shipped parrot at 11. Only 1 cm carries a bill, a two-voxel eye, a neck and a
crest across the whole set, and it nests 10:1 in the terrain lattice and 5:1 in
the asset lattice.

A goldcrest is 9 cm and a wren is 10. **Four species are authored above life
size** — european-robin (24 cm against 14), great-tit (24 against 14),
barn-swallow (26 against 17–19) and common-kingfisher (20 against 17) — and each
says so in its own notes. **Three of them sit above the 20 cm floor rather than
at it**, because a perched songbird is authored at 36–42 degrees nose-up and
20 cm of bird at that angle projects onto sixteen voxels of length.
`tools/birdprobe.py --read` measures the animal's longest axis rather than its x
extent for exactly that reason, and it flagged all three as SHORT at 20 cm.

### Detail entities

Same promise the fish make: **nothing about an individual is saved.** It is
spawned from `(species, seed)` when the player is near and it is gone when it
despawns. The `flock` group is the specification a spawner will be written
against, stated in the same file the shape is stated in so the two cannot drift.
Nothing reads it yet.

It is a separate group from the fish `detail` group because five of that group's
eleven rows are about water depth, and a bird does not have one. What a bird has
instead is `flock.perch` (ground, shrub, canopy, cliff, waterside, water, air),
a flying height band, and `flock.flight_share` — how often the species is in the
air rather than perched, which is what tells a spawner which POSE to ask for. A
vulture is 0.90 and a wren is 0.05.

### Keyword input

`forge/language.py`, still fully local — no network, no model. **46 bird species
keywords**, and each is a WHOLE BIRD: typing `heron` replaces the five length
shares, the posture, the bill, the tail and the legs together, because a heron
with a songbird's neck is a grey songbird.

    heron        eagle        raven        pigeon       kingfisher
    robin        swallow      owl          duck         macaw
    wren         swift        falcon       vulture      hummingbird
    ... 46 in all, plus 74 shape and colour phrases

Species keywords set SHAPE only. Colour is a separate vocabulary
(`iridescent`, `turquoise bird`, `sandy bird`, `glossy black`) so that `raven`
and `glossy black` compose instead of one overwriting the other.

Three phrases mean the same thing to a fish and to a bird — `square tail`,
`rounded tail`, `barred` — and they set **both** parameters. A fish spec never
reads `bird.*` and a bird spec never reads `fish.*`, so the half that does not
apply is a no-op. The bird switches live in their own table and are merged by
`_merge_switches`, which records a collision instead of overwriting: a Python
dict literal with a duplicate key keeps the last one and says nothing, and
written into one literal the bird entries silently replaced the fish ones.

### Proving the sliders do something

    python tools/birdprobe.py             # every parameter, 4 seeds averaged
    python tools/birdprobe.py --read      # silhouette, contrast, erosion
    python tools/birdprobe.py --lattice   # 1 cm against 2 cm and 5 cm
    python tools/birdprobe.py --pose      # folded against spread, one piece, one bird
    python tools/birdprobe.py --pose-ab   # the pose A/B render
    python tools/birdprobe.py --sex       # male against female, in voxels
    python tools/birdprobe.py --sex-ab    # the sex A/B render

Same idea as `fishprobe.py` and a harder problem, because a bird is six parts
and a measurement taken off the whole animal is usually dominated by the wrong
one — the tail is longer than the body, the wings are wider than everything, and
the bill is four voxels out of six hundred. **Most of the measurements read the
material histogram or a region of the silhouette, and each one says what it was
measuring before that turned out to be the wrong part.**

It found four real defects in the generator and six in itself. The
generator's: `bill_gape` multiplied by `bill_depth`, so a shallow bill could not
be made wide and the slider did nothing on any bird smaller than a heron; the
folded wing covered five sixths of every animal (119 wing voxels against 88 of
body on a robin); every head came out twice the length its own share asked for,
because a `0.5 × … × 2.0` cancelled; and the eye was silently not drawn at all
on about a third of the small-headed species, because the computed station
rounded to a cell just outside the head.

Its own included counting a herring gull at **74% ink** on a species that
carries no marking at all — the head-mark slot happened to hold the same grey
its back is painted in. The fix is that ink is now measured as the difference
between the bird and the same bird with its markings turned off, which measures
the marking whatever colour it shares.

The `--read` pass is the one to run before approving a species. Its contrast
floor is **2.0** where the fish probe's is 1.5, and the difference is the brief:
at 1.5 a marking is present and faint; at 2.0 it is a block of colour.

It checks **four** contrasts, not one: the wing mark against the wing, the body
mark against the underparts, the head mark against the head, and **the bill
against the head**. The fourth was added last and it was the most productive:
**ten of the twenty species were drawing a bill in a colour that did not
separate from the head it sticks out of** — a robin's horn bill on its olive
head measured 1.04, a great tit's black bill on its black cap measured 1.00.
The bill was there in every case and could not be seen. Twelve of the twenty
now carry a bright bill and three a grey one.

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
    tafoni-sandstone     3.6 m  hollowed by cavernous weathering
    banded-sandstone-ledge 4.2 m stepped ledge, differential erosion
    jointed-granite-tor  4.0 m  faces sharing three joint orientations
    fractured-outcrop    5.5 m  separate blocks with the joints standing open
    basalt-colonnade     4.5 m  vertical columns from a Voronoi crack network
    exfoliating-dome     5.0 m  concentric shells spalling off a dome

The last six are not re-tunings of the granite boulder — they are rock classes
the generator could not previously make, one per mechanism:

    tafoni-sandstone        rock.cavernous     runaway pitting
    banded-sandstone-ledge  rock.bedding       differential erosion
    jointed-granite-tor     rock.joint_sets    shared fracture orientations
    fractured-outcrop       rock.block_relief  open joints between blocks
    basalt-colonnade        rock.columns       cooling-contraction network
    exfoliating-dome        rock.exfoliate     shells parallel to the surface

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

### Fish  (voxel size per species)

    1 cm   brown-trout  river-perch  pale-minnow  river-eel
           northern-pike  mud-catfish  golden-carp
           clown-anemonefish  reef-tang  shoal-herring
    2 cm   reef-shark  bluefin-tuna  mirror-carp
    5 cm   great-white-shark  tiger-shark  whale-shark
           scalloped-hammerhead

### Whales & dolphins  (voxel size per species)

    2 cm   common-dolphin
    5 cm   bottlenose-dolphin  orca  beluga
    10 cm  humpback-whale  sperm-whale  blue-whale

**The voxel size scales with the animal**, and the reason is the opposite of
the obvious one: a big animal needs MORE voxels of length than a small one,
because the features that identify it are a smaller fraction of its length. A
reef fish's dorsal fin is 25% of its body; a dolphin's is 10%; **a blue
whale's is 1.2%**. The rule is *choose the coarsest authorable voxel size at
which the species' smallest identifying feature is still about three voxels
across*, and what it produced is a band of 28 to 294 voxels of length across
the whole set. `docs/marine-megafauna-research.md` §5 has the argument and
`python tools/fishprobe.py --lattice` has the table.

No species is authored under **20 cm** (owner, 2026-08-13). Below that a
marking cannot be two voxels wide; enlarging the animal was chosen over adding
a lattice tier finer than 1 cm, so `clown-anemonefish` is bigger than a real
one and its spec notes say so.

### Birds  (1 cm)

    common-raven         eurasian-jay         european-robin
    great-tit            song-thrush          common-starling
    eurasian-hoopoe      great-spotted-woodpecker
    tawny-owl            golden-eagle         common-buzzard
    common-kestrel       barn-swallow         herring-gull
    grey-heron           common-kingfisher    mallard-duck
    rock-pigeon          rock-ptarmigan       scarlet-macaw

Fifteen are authored perched and five flying: the eagle, the buzzard, the
kestrel, the swallow and the gull, which are the five whose wing planform is the
point. **Every one of the ten biomes carries at least one**, including the two
that hosted nothing but fish and boulders — open ocean gets the gull and bare
rock gets the raven, the eagle, the kestrel, the pigeon and the ptarmigan.

The set is chosen to span the readable range of SHAPES, not ornithology: heron
against starling on neck, macaw against kingfisher on tail, woodpecker against
mallard on posture, owl against heron on head size, and all four wing planforms.
See **Birds** above for why four of them are authored above life size.

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
    forge/fish.py      the fish generator; a lofted body plus thin fin plates
    forge/render.py    isometric preview renderer (numpy + Pillow, no engine)
    forge/contact.py   contact sheets
    forge/vox.py       MagicaVoxel .vox export, splitting models over 256 voxels
    forge/vxa.py       VXA1, the compact native format the engine will read
    forge/materials.py material IDs -- slots 0-15 mirror core.h, 16+ are proposed
    forge/palette.py   GENERATED -- what each material looks like, straight from
                       the engine's materialpalette.h. Never edit it by hand
    forge/web/         the app; hand-written, no framework, no vendored library
    tools/gen_palette.py rewrite forge/palette.py from the engine header; the
                         selftest re-runs it in memory and fails if you forgot
    tools/healthpass.py  build every spec at N seeds and print the health checks
    tools/sheet.py       contact sheet of named specs, for eyeballing shape
    tools/shapecheck.py  same, every voxel forced to one bright material
    tools/shots.py       responsive-layout screenshots via headless Chrome
    tools/lattice_ab.py  a plant authored for 2 cm beside the same one authored
                         for 5 cm, drawn at matched physical scale
    tools/retune_5cm.py  one-off that moved the last seven off-lattice assets
    tools/all_to_5cm.py  one-off that put every remaining species on 5 cm
    tools/seed_boulders.py one-off that authored the 4.5-9 m boulders
    tools/seed_rocktypes.py one-off for the six geology-driven rock classes
    tools/seed_kinds.py  one-off that authored the rock/bush/gap species
    tools/seed_ground.py one-off that authored the ground-cover species
    tools/seed_fish.py   one-off that authored the ten small fish species
    tools/seed_marine.py one-off that authored the sharks, whales and dolphins
    tools/fishprobe.py   sweeps every fish slider and says DEAD when one does
                         nothing; also the readability and lattice checks

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

Before calling a change done, run all three:

    python tools/healthpass.py 1 2 3     # every species, three seeds, flagged
    python -m forge.cli selftest         # determinism, connectivity, round trips,
                                         # that palette.py is still generated from
                                         # the engine header, that every build
                                         # is ONE connected piece, and that every
                                         # spec still SAYS what it builds — an
                                         # out-of-menu choice is replaced with the
                                         # parameter's default and the replacement
                                         # is what gets saved, so a bogus value is
                                         # tripped through five menus every run to
                                         # prove the warning still fires
    python tools/buildcheck.py           # builds every spec in specs/ and fails on
                                         # a second piece, a spec warning, a health
                                         # problem, or a build that raises

### Open: `hero-sequoia` sheds wood on most of its individuals

`python tools/buildcheck.py` currently FAILS on one spec, and it is worth being
precise about what it is and what it is not.

    hero-sequoia seed 1: broken: 3235 wood voxels are not joined to the trunk

**Measured, not guessed.** At 20 cm, on seed 6: the tree's 348,274 wood voxels
form ONE 26-connected component and 73 of them touch the trunk only at a CORNER
rather than face to face. Nothing floats; the asset is one piece. What fails is
the `wood connected` check, which is face-only on purpose — "a branch joined to
the trunk only at a corner is a branch that falls off".

**It is not new and it is not a fish problem.** Under the spec hash this species
had before the fish parameters existed, it sheds wood on **five of eight**
individuals; seed 1 happened to be one of the three clean ones, and seed 1 is
the only individual `buildcheck` ever builds. Adding any row to `spec.PARAMS`
changes every spec's hash, and the hash is mixed into the seed, so every species
in the library moved to a different individual — this one landed on a bad one.

That is the finding, and it is a bigger one than the tree: **the library has been
green on one seed per species, which is a sample of one.** `--seeds 1 2` over
everything but the four heroes is 146 builds and passes; the heroes need
their own multi-seed pass and nobody has run one.

Reproduce:

    python -c "from forge import pipeline, spec as sm; s,_=sm.load('specs/hero-sequoia.json');       [print(i, pipeline.build(s,i,resolution_cm=20).stats['wood_detached']) for i in range(1,7)]"

`buildcheck.py` is what CI runs, and it is the one that catches the defect class
this generator keeps producing: an asset that builds, reports success, and ships
in several pieces. `selftest` covers the same rule on the specs that are cheap to
build; four heroes are too large for a pre-commit check and need
`python tools/buildcheck.py --only-heavy`.

ONE GENERATION MAKES ONE ENTITY — one rock, one tree, one clump of grass. Small,
medium and large stones are separate species, generated separately and placed
together by placement logic later. A build that hands back a stone plus a ring of
loose rubble has produced several things, and nothing downstream can tell which
of them was the asset.
