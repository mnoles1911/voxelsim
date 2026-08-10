# Voxel tree asset generator — prior-art research

Date: 2026-08-09
Status: research only. No code written, no decisions committed.

Goal being researched: a bespoke desktop/local app that generates cubic-voxel
tree assets by the hundreds, takes designer feedback through sliders and plain
language, and lets the designer keep the good ones for later use in the game
(possibly via Blender in between).

---

## 1. What the engine already forces on the design

Checked directly, not assumed:

- **Voxels are 10 cm.** `voxel-core/include/voxelcore/core.h:263` —
  `kVoxelSizeMm = 100`.
- **The material palette is 16 slots and every one is terrain.**
  `core.h:304-333` — air, bedrock, rock, gravel, sand, subsoil, topsoil, snow,
  grass, jungle soil, savanna grass, podzol, permafrost, mud, clay, watermark.
  There is no bark, wood, leaf, needle, or deadwood material. Adding them is a
  prerequisite for any tree that the existing renderer can draw.

Two consequences that rule things in and out:

**Trees are big in voxels.** At 10 cm a 12 m tundra pine is 120 voxels tall and
maybe 40 wide. A 30 m jungle emergent is 300 voxels tall. That is one to two
orders of magnitude more voxels than the 16³–32³ props most voxel tooling is
built around, and 300 exceeds the 256-per-axis cap in MagicaVoxel's `.vox`
format (a scene works around it by splitting into several models). So `.vox` is
fine as an interchange format for small and medium trees and for the Blender
round trip, but it cannot be the native format for the largest trees without
splitting.

**Branches are near the resolution floor.** A real branch 6 cm across is smaller
than one voxel. Anything thinner than ~15 cm will either vanish or snap to a
full 10 cm cube. This is the single most important technical fact for choosing
an approach, and it is what makes the obvious path (generate a nice mesh, then
voxelize it) worse than it looks — see §4.

---

## 2. Direct hits: voxel-native tree generators

### treegen-pinegen (NGNT) — closest thing to the app being described
<https://github.com/NGNT/treegen-pinegen>

MIT. Python 3.8+, NumPy / Pillow / PyQt6. Five generators — Treegen (oak-like),
Pinegen (conifer), Birchgen, Palmgen, Kapokgen (broadleaf tropical, with roots
and canopy controls). Each exposes 8–20 sliders (size, trunk dimensions, spread,
twist, leafiness, gravity, seed). Writes MagicaVoxel `.vox` with 256-colour PNG
palettes. Separate worker module per tree type, so adding a sixth is mechanical.

This is essentially a small prototype of the requested app. Worth reading in
full before writing anything. Caveats: unvetted single-author project, PyQt6
desktop UI (awkward for the "show me 100 thumbnails and let me pick" workflow),
and it has no notion of feedback loops or a saved library.

### voxel/voxel-trees
<https://github.com/voxel/voxel-trees> — standalone generator for several voxel
tree types, from the old voxel.js ecosystem. Old, small, but a source of simple
species recipes.

### VoxelTree (MagicaVoxel shader)
<https://voxeltree.wordpress.com/> — grows trees inside MagicaVoxel using its
shader scripting. Interesting as a demonstration; not automatable at the scale
wanted here.

---

## 3. Mature tree algorithms (mesh-first, but the maths is what we want)

These are the two algorithms that essentially all serious tree tools use.

### Weber–Penn parametric model (1995)
Paper: <https://courses.cs.duke.edu/cps124/fall01/resources/p119-weber.pdf>

The industry-standard parametric description of a tree: recursive levels of
branches, each level with its own length, taper, curvature, split angle, and
child count, plus a global crown envelope. It is a *parameter schema* as much as
an algorithm — which is exactly what a slider UI needs. Implementations:

- **Arbaro** (Java, GPL) — <https://github.com/wdiestel/arbaro>. Reads XML
  parameter files. Ships a library of species presets.
- **Sapling Tree Gen** (Python, GPL, bundled with Blender) —
  <https://docs.blender.org/manual/en/3.3/addons/add_curve/sapling.html>.
  Same model, scriptable, and Blender runs headless (`blender -b -P script.py`),
  so it can be driven in batch. Comes with free species presets.

Strength: predictable, controllable, well-suited to conifers and formal shapes.
Weakness: recursive-by-construction, so irregular real-world crowns are hard.

### Space colonization (Runions et al., 2007)
Paper: <https://algorithmicbotany.org/papers/colonization.egwnp2007.large.pdf>

Scatter attraction points in a crown volume; branches grow toward whichever
points they are nearest to; points are consumed as they are reached. Competition
for space produces the branching pattern rather than a recursion rule. It is
excellent at the irregular deciduous crowns Weber–Penn struggles with, branches
never intersect by construction, and the crown shape is controlled simply by the
shape of the point cloud you scatter — which means "desert dead tree", "jungle
emergent with a flat top", and "wind-swept tundra pine" are different point
clouds, not different code.

It is also *small* — a working 3D implementation is a couple hundred lines.
Reference implementations: <https://github.com/bebbo203/procedural_tree_generator>
(C++, follows the paper), <https://github.com/dsforza96/tree-gen>,
<https://github.com/MFry/Procedural-Trees>, and a clear write-up at
<https://ciphrd.com/articles/generating-a-3d-growing-tree-using-a-space-colonization-algorithm/>.

### Other mesh generators worth knowing
- **EZ-Tree** (dgreenheck, MIT, JavaScript/TypeScript, three.js) —
  <https://github.com/dgreenheck/ez-tree>. Importable as a library, parameters
  live in a structured `TreeOptions` object (bark / branch / leaves groups) with
  named presets, exports GLB. The cleanest example of "parameters as data" to
  copy. (Note: some third-party write-ups describe EZ-Tree as a Python/Apache
  tool — that is wrong; the repo is JS/MIT.)
- **proctree / proctree.js** (jarikomppa, liberal licence, C++ and JS) —
  <https://github.com/jarikomppa/proctree>. Fast, with a small editor.
- **Modular Tree / MTree** (Blender addon, GPL, node-based) —
  <https://extensions.blender.org/add-ons/modular-tree/>. L-system growth with
  apical dominance, gravity and stiffness, procedural leaves via the
  superformula. Good source of ideas for making trees droop and lean believably.
- **Tree It** (free, closed source) — exports OBJ/FBX. Useful as a reference for
  what a designer-facing tree UI feels like.

---

## 4. The voxelization bridge — and why I would not lean on it

Tools exist and are good:

- **trimesh** — `voxelize()` with `subdivide` / `ray` / `binvox` backends, plus
  `fill()` morphology for hollow interiors.
  <https://trimesh.org/trimesh.voxel.creation.html>
- **cuda_voxelizer** (Forceflow) — command-line, writes `.vox` and `.binvox`
  directly, hundreds of times faster than CPU.
  <https://github.com/Forceflow/cuda_voxelizer>

The tempting plan is: generate a beautiful mesh with a mature tool, voxelize it,
done. The problem is §1 — at 10 cm, twigs are thinner than a voxel. Voxelizing a
mesh gives no control over what happens to them: surface voxelization leaves
branches hollow and broken, solid voxelization fattens them unpredictably, and
either way the tree can end up with a crown that is not actually connected to
its trunk. You would then spend the project fighting a repair step.

Generating **directly into the voxel grid** avoids this entirely: you keep the
tree as a skeleton (a set of branch curves with radii), and rasterize each
segment as a voxel capsule with an enforced minimum radius of one voxel. Every
branch is guaranteed connected and guaranteed visible, because you decided so,
not because a mesh happened to survive sampling.

So: **take the algorithms from §3, not their meshes.** Voxelization stays in the
toolbox for the Blender round trip (designer edits a tree as a mesh, we bring it
back), not for the main generate path.

---

## 5. AI / learned-model options

Three distinct things get called "AI trees"; they are not interchangeable.

**Text/image → 3D mesh (TRELLIS, Hunyuan3D 2.0/3.5).** State of the art and
genuinely impressive. TRELLIS internally builds a coarse 64³ voxel structure
then refines it — <https://arxiv.org/html/2501.12202v5>. But output is a
one-off textured mesh with no parameters, no seed-family, no palette
constraint, and no guarantee of consistency across a hundred variants. It
inherits every problem in §4 plus non-determinism. Verdict: useful for
*inspiration blockouts* ("what does a Joshua-tree-ish silhouette look like"),
not for production assets.

**Learned voxel generation.** *Scaffold Diffusion: Sparse Multi-Category Voxel
Structure Generation with Discrete Diffusion* —
<https://arxiv.org/pdf/2509.00062> — generates sparse voxel structures with
per-voxel categories directly, which is the right output shape for us. This is
the interesting long-game option: once there is a library of a few hundred
designer-*approved* trees, that library is training data, and a model can
propose new ones in the established style. It is a phase-3 idea, not a starting
point — it needs the library to exist first.

**LLM as the parameter driver — this is the one that fits the request.** The
natural-language part of the ask is best served by having the model write and
edit the *parameter spec*, never the voxels. Precedent:
- *Proc3D* — <https://arxiv.org/html/2601.12234> — natural language to a
  procedural representation, where users then make parametric edits with sliders
  bound to the same parameters, or issue further language edits. Exactly the
  dual slider/language control being asked for.
- Hesiod's LLM-driven terrain generation — an LLM turns a description into a
  compact JSON spec of nodes, parameters and links.
  <https://hesioddoc.readthedocs.io/en/latest/guides/llm-procedural-generation/>
- *Zero-shot 3D Map Generation with LLM Agents* —
  <https://arxiv.org/pdf/2512.10501> — an actor agent proposes parameters and a
  critic agent evaluates and refines, iteratively.

The design consequence is a good one: **one JSON species spec is the pivot.**
Sliders write it, language edits patch it, batch generation reads it, seeds vary
within it, and the library stores it. Nothing else in the app needs to know
where a change came from.

---

## 6. Recommended shape (for discussion, not committed)

1. **Species spec = JSON.** Crown envelope, trunk profile, branch levels, droop,
   leaf clumping, material assignments, plus a seed. Weber–Penn's parameter
   names are a good starting vocabulary.
2. **Skeleton generator = space colonization**, with the crown envelope driving
   the attraction-point cloud, and Weber–Penn-style parameters shaping trunk and
   branch behaviour. Roughly 300–500 lines, no dependency on any of the projects
   above, but informed by all of them.
3. **Rasterize the skeleton straight to a 10 cm voxel grid**, minimum branch
   radius one voxel, then place foliage as clumps around branch tips. Guarantees
   connectivity; see §4.
4. **Materials:** needs new palette entries (bark, heartwood, deadwood, broadleaf,
   needle, at minimum) before anything renders in-engine.
5. **App = local web UI.** The core interaction wanted is "generate 100, show me
   a wall of thumbnails, let me keep six" — a browser grid with a three.js
   preview does that far more cheaply than a desktop toolkit.
   treegen-pinegen's PyQt6 approach is the main thing I would *not* copy from it.
6. **Feedback loop:** sliders and language both produce patches to the same spec.
   Keep an accepted-library on disk; it is both the deliverable and, later, the
   training set for §5.
7. **Export:** `.vox` for the Blender/MagicaVoxel round trip (splitting models
   above 256 voxels per axis), plus a compact native format for the engine.

---

## 7. Decisions (owner, 2026-08-09)

- **Trees are real voxels in the world, destructible and diggable.** Not placed
  meshes. Rendering may end up ray-marched. This is the demanding option and it
  makes §1 binding: 10 cm grid, real material IDs, and the streaming and mining
  paths both have to accept them.
- **Heroes plus seeded filler.** Hand-approved standout trees for landmarks and
  near-camera work, plus seeded variation for background forest.
- **Lives in a new folder inside `D:\voxelsim`.**

Build plan: `docs/tree-asset-generator-plan.md`.

## 8. What the "real voxels" decision costs — two things found in the code

**A. Adding materials is easy, but it has three tails.** `MaterialId` is
`uint8_t` and IDs are documented append-only (`core.h:297-302`), so there is
room for bark, heartwood, deadwood, broadleaf and needle without touching the
storage format. `docs/m4-plan.md:83` records a previous append (8→15), so the
move is precedented. But appending touches:
- `VoxelAgentSubsystem.cpp:64` — `kMineCostByMaterial[vxc::kMaterialCount]` with
  a `static_assert` on its length. Every new material needs a mining cost, which
  for trees is exactly right: chopping a trunk should not cost what digging
  bedrock costs.
- `mips.h:60,77` — per-material counting arrays in the LOD reduction, sized by
  `kMaterialCount`. They resize for free but the majority rule now has more
  candidates.
- `test_mesher.cpp:123-132` — an explicit warning that its synthetic bricks must
  **not** track `kMaterialCount`, because appending a material silently changed
  the golden digests once before. Read that comment before appending.

**B. The real blocker: the world skips chunks it proves are empty.**
`voxel-core/bench/terrainprobe.cpp:666-669` states it plainly — `surfaceUpperBoundMm`
and friends are *safety primitives*: "the streaming layer skips generating any
chunk they prove empty or fully solid, so a bound that is too tight is not a
lost optimisation, it is terrain that never generates. A hole in the world."

A tree is solid **above** the terrain surface, so a 12 m pine puts solid voxels
in chunks that the surface bound currently proves empty. Those chunks are
skipped, and the crown simply never generates. `core.h:322-333` confirms this is
a known invariant: `MAT_WATERMARK` is called out as "the one thing in this enum
that is SOLID ABOVE THE SURFACE", and the comment says the amplifier's bound
soundness argument depends on that being the only exception.

So before any generator work is worth doing, the surface upper bound has to
learn about vegetation — most likely by raising it by a per-biome maximum tree
height wherever trees can be placed. That is the first thing to prove, and it is
cheap to prove with a hand-authored box, long before there is a generator.

## 9. Second pass: what we can actually take, and under what licence

Done 2026-08-09 after `asset-forge` Phase 1 was working, specifically to find
code worth harvesting rather than algorithms worth reading.

### The licence picture is the headline

The *mature* tree tools are copyleft, and that is the reason the small MIT ones
matter:

| Project | Licence | Usable in our tool? |
|---|---|---|
| **vengi** (voxel tools) | **MIT** (Copyright (c) 2023 Martin Gerhardy) | yes |
| **treegen-pinegen** | **MIT** (Copyright (c) 2025 NGNT) | yes |
| **voxel/voxel-trees**, substack/voxel-forest | MIT | yes, but tiny and abandoned |
| Blender **Sapling Tree Gen** | GPL-3.0-or-later | algorithm yes, code no |
| **Arbaro** | GPL | algorithm yes, code no |
| **Modular Tree / MTree** | GPL | algorithm yes, code no |
| **joesobo/ProceduralVoxelTree** | **none at all** | **no** |

Two practical notes. Weber-Penn and space colonization are *published papers*,
so implementing from the paper is the ordinary route and is what `asset-forge`
already does — the constraint is on copying GPL *source*, which would pull the
generator itself under GPL. And `joesobo/ProceduralVoxelTree`, which looks like
the closest match on paper (space colonization, voxel output), has **no licence
file** — last pushed 2020, 5 stars. No licence means all rights reserved, so it
cannot be copied from at all regardless of how apt it looks.

### The find: vengi

<https://github.com/vengi-voxel/vengi> — MIT, 1,392 stars, 109 forks, actively
maintained (last push 2026-07-31), C/C++ with a Lua scripting layer.

It is a voxel editor and format converter, but the part that matters to us is
`src/modules/voxelgenerator/lua/scripts/`, which holds **23 named tree species
as Lua scripts**:

    tree_americanbeech  tree_americanelm  tree_araucaria   tree_balsamfir
    tree_baobab         tree_birch        tree_blackwillow tree_bonsai
    tree_branchellipsis tree_butternut    tree_cherry      tree_cone
    tree_cube           tree_cubesidecubes tree_dome       tree_domehanging
    tree_ellipsis       tree_fir          tree_hawthorn    tree_palm
    tree_pine           tree_redmaple     tree_tulip

plus `grass.lua`, `mushroom.lua`, `potplant.lua`, `desertrocks.lua`,
`clouds.lua` — which covers most of the non-tree scatter list too.

They share a `modules/tree_utils.lua` with exactly the primitives a voxel tree
generator needs: `drawBezier`, `bezierPointAt`, `createCurvedTrunk`,
`createBaseFlare`, `createLineRoots`, `createCanopyDome`, `leafCluster`. There
is also an `LSystem.cpp` in the same module.

Two scripts read in detail:

- **`tree_americanbeech.lua`** — hierarchical branching. Bezier trunk with
  lean, base flare and surface roots (beeches are shallow-rooted), main
  branches with sub-branches and tertiary twigs, leaf clusters at several
  levels. 15 parameters with explicit ranges: trunk height 6-30, thickness 1-6,
  lean 0-4, branches 3-10, branch length 4-18, sub-branches 1-6, canopy spread
  5-22, density 1-5, a low-sweeping-branches toggle, seed, and four colours.
- **`tree_pine.lua`** — deliberately *not* the same method. Layered branch
  whorls: crown start as a percentage of trunk height, 3-14 whorls, 3-8
  branches each, base and top radius, droop, needle cluster size,
  irregularity, dead stubs below the crown.

That difference is the useful lesson. A conifer's whorls are a real botanical
structure, and generating them explicitly gives a better pine than asking a
space-colonization crown to approximate one. Our generator currently uses one
method for every species.

### What is worth taking, and what is not

`asset-forge` should not restart on top of any of these. None of them know
about 10 cm voxels, `vxc::Material`, face-connected wood for a destructible
asset, `(spec, seed)` determinism, or batch review — that is the part already
built and it is the part specific to us. What we are short of is **content**:
six species against a target of hundreds. That is exactly what vengi has.

Worth harvesting, in order of value:

1. **The 23 species recipes as parameter data.** Not the code — the numbers.
   Each script encodes a real species' proportions with sensible ranges, and
   deciding "what makes a hawthorn a hawthorn" is the slow part of authoring
   hundreds of specs. This is a direct answer to the content problem.
2. **A whorl skeleton backend**, ported from `tree_pine.lua`'s approach, as an
   alternative to space colonization selected by a spec field. Conifers, palms
   and araucaria all want it. This is the single biggest quality gain available.
3. **`tree_utils.lua` primitives** — base flare, line roots and the bezier
   trunk are better than what we have. Our trunk wanders by random walk; a
   bezier gives the designer a controllable curve instead.
4. **vengi as an independent format check.** It loads and saves many voxel
   formats. Right now the only thing that verifies our `.vox` output is our own
   reader, which is a closed loop — vengi opening the file would be real
   evidence.
5. **treegen-pinegen** (MIT, 6 stars, 40 commits, PyQt6) — small, and less
   useful now that we have a working generator, but its palm and kapok recipes
   are worth reading for the same reason as (1).

Caution before copying anything: the repo LICENSE is MIT, but a few files in
`modules/` are clearly third-party (`stb_aos.lua`, `JSON.lua`, `perlin.lua`).
Check per-file headers rather than assuming the root licence covers them, and
keep an attribution note for anything ported.

## Sources

- <https://github.com/NGNT/treegen-pinegen>
- <https://github.com/voxel/voxel-trees>
- <https://voxeltree.wordpress.com/>
- <https://courses.cs.duke.edu/cps124/fall01/resources/p119-weber.pdf>
- <https://github.com/wdiestel/arbaro>
- <https://docs.blender.org/manual/en/3.3/addons/add_curve/sapling.html>
- <https://algorithmicbotany.org/papers/colonization.egwnp2007.large.pdf>
- <https://github.com/bebbo203/procedural_tree_generator>
- <https://github.com/dsforza96/tree-gen>
- <https://github.com/MFry/Procedural-Trees>
- <https://ciphrd.com/articles/generating-a-3d-growing-tree-using-a-space-colonization-algorithm/>
- <https://github.com/dgreenheck/ez-tree>
- <https://github.com/jarikomppa/proctree>
- <https://extensions.blender.org/add-ons/modular-tree/>
- <https://trimesh.org/trimesh.voxel.creation.html>
- <https://github.com/Forceflow/cuda_voxelizer>
- <https://arxiv.org/html/2501.12202v5>
- <https://arxiv.org/pdf/2509.00062>
- <https://arxiv.org/html/2601.12234>
- <https://hesioddoc.readthedocs.io/en/latest/guides/llm-procedural-generation/>
- <https://arxiv.org/pdf/2512.10501>
- <https://magicavoxel.fandom.com/wiki/World_size>

Second pass (section 9):

- <https://github.com/vengi-voxel/vengi> and its
  `src/modules/voxelgenerator/lua/scripts/` tree scripts
- <https://vengi-voxel.github.io/vengi/voxedit/Features/>
- <https://github.com/joesobo/ProceduralVoxelTree> (no licence file)
- <https://github.com/substack/voxel-forest>
- <https://extensions.blender.org/add-ons/sapling-tree-gen/> (GPL-3.0-or-later)
- <https://www.gamedevhub.dev/guides/open-source-licenses/>
