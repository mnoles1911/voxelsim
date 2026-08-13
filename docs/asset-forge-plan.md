# Environment assets: what is left, in the order it should be done

Written 2026-08-11. Companion to `docs/backlog.md` §8, which holds the standing
context; this is the sequenced plan and the acceptance test for each item.

**Where things stand.** Sixty-six species build, every one on the 5 cm lattice.
None of them is in the world, nothing in the game reads the colour table, and
the last three sessions have been spent on defects that all failed the same
way — they ran, reported success, and were wrong. That is the thing to keep in
mind while reading the order below: the sequencing is not by size, it is by
*how much later work a mistake here would poison*.

Every item says how you will know it is done. An item without a check is an
item that will be declared finished twice.

---

## Phase 1 — Finish the rock defects

These are wrong right now, in assets that are otherwise called done. All are
measured, none are speculative.

**1.1 `hero-tor-stack`: 297,000 voxels floating (6.5%, four pieces).**
A tor stack is boulders resting on each other; four of them rest on nothing.
Only visible since the support check began reporting large unsupported pieces
instead of deleting them. *Done when* `airborne_kept` is empty across three
seeds and the stack still reads as stacked in a side elevation — not the
isometric preview, which cannot show it (see 1.6).

**1.2 `limestone-pinnacles`: 71% of the asset floats.**
Pre-existing and unrelated to the plinth work — measured 71.3% before that
change and 67.1% after, so the plinth is not the answer here. Cause unknown.
It is a small non-hero, which is why it sits below the tor stack. *Done when*
the cause is named and `airborne_kept` is empty across three seeds.

**1.3 `hero-arch-colossal` has no entry in `tools/seed_heroes.py`.**
Its hand-tuned values exist in exactly one place, so the file that regenerates
every other hero would silently drop it. That generator was destructive until
this session — running it reverted a finished arch to draft values. *Done when*
the spec round-trips through the generator into a scratch directory unchanged,
which is the check that caught the other six.

**1.4 `rock.size_m` is fitted against the grid including the rubble ring.**
A spec with lots of rubble spends its size budget on debris: the tor asked for
17 m and the stone was 9.8 m until rubble was cut to 0.3. Worked around per
spec, which means the workaround is now baked into several specs.
*Done when* size is measured against the main body and the affected specs have
their rubble put back. **This resizes much of the library, so it must land with
before/after renders**, not just numbers — and probably after Phase 2, so the
resize is protected by a test.

**1.5 Review the 16 legacy rock species.**
They were tuned around a weathering pass that removed 20 voxels from a stone of
90,000, so their shapes were won with `rough` and cut planes standing in for
erosion that never ran. Erosion now works and scales with size.
*Done when* each of the sixteen has been looked at and either re-tuned or
recorded as correct as-is. **Judge with `tools/waistprobe.py`, not
`rockmech.py`** — the refit launders mass changes into whole-surface shifts, so
that harness reads large whether or not the mechanism did anything shaped.

**1.6 The preview camera cannot show a stack or an overhang.**
`hero-tor-stack` and `hero-balanced-rock` both read wrong in the isometric
preview and right in side elevation, because the camera looks down onto the
thing that matters. This is a review-tooling defect, not an asset defect, and
it has already caused one wrong verdict. *Done when* the contact sheet and
turntable default to a side elevation for rocks, or carry one alongside.

---

## Phase 2 — Make the fixes stay fixed

Cheap, and it goes before the expensive work rather than after, because
everything below changes many assets at once and there is currently nothing
that would notice a regression.

**2.1 asset-forge has no CI at all.**
`python -m forge.cli selftest` is named in the README as one of two things to
run before calling a change done, and nothing enforces it. `ci.yml` has jobs for
voxel-core, the unity lint, the shader lint, terrain-service and a Docker build,
and zero mentions of asset-forge. *Done when* a CI job runs `selftest` plus a
build of every spec, and fails on a floating piece or a validation warning.

**2.2 Put the floating-piece check in `selftest`.**
`airborne_kept` is reported per build and read by nobody. The three defects in
Phase 1 were all found by hand. *Done when* selftest builds the rock library and
fails on any unsupported piece over the size threshold.

**2.3 The palette drift check already runs in CI** via
`tools/compile-shaders.ps1`, which the "Shader compilation" job calls. Nothing
to do; recorded so nobody adds a second one.

---

## Phase 3 — Get the colour into the game

**3.1 Wire the palette into the renderer.** The table is generated, guarded and
compiling (backlog §8 has the detail). What is left is `TexCoords[3]/[4]` at
pixel rate in `VoxelQuadVertexFactory.ush` and an `M_VoxelTerrain` graph change
to read them, as `BaseColor = lerp(biomeAlbedo, paletteRGB, isAsset)`.
Writing it is code; *running* the graph generator is an editor commandlet, so
verification is blocked on the box. **Do not replace the biome path wholesale**
— it is the only appearance path that currently works and the swap cannot be
verified in the same motion. *Done when* a bark voxel and a leaf voxel render
in their palette colours and terrain is pixel-identical to before.

**3.2 Generate `terrain_palette.py`'s colours from the header.** It is a second
palette — 16 entries, stopping at `MAT_WATERMARK`, its own docstring calling it
"single source of truth" — and the ten asset materials have no appearance on the
UE side at all. It should keep owning `BIOME_TINT`, which genuinely is UE-side
policy, and take its RGB from the engine. *Done when* the drift check covers it
too. Follows 3.1, because until then nothing reads the result.

---

## Phase 4 — Get assets into the world

**4.1 UE wiring for asset streaming. Blocked on the editor box.** The
voxel-core half is largely written; `assetplacement.h` gives the provable reach
bound that lets the streaming path keep skipping chunks it can prove empty. What
remains is getting a baked asset into the volume the marcher reads and
confirming a crown lands in the chunks the bound predicted. Neither half can be
*verified* without the editor, and one editor per box is a hard rule here — two
capture sessions on one machine cost hours once and read exactly like a slow
configuration. **Unblocks: the box, nothing else.**

---

## Phase 5 — Foliage, where the remaining calls are yours

The mechanical work is done: all 21 trees and bushes carry a leaf habit, the
habit changes shape rather than mass, and visible crown wood sits at 12–15% for
the broadleaves. What is left is judgement and one honest gap.

**5.1 `savanna-acacia` shows 32% wood, twice its neighbours.** Tuned airy on
purpose, and a flat-topped acacia really does show its armature. It is the one
number that looks wrong beside the others. *Needs your eye, not a measurement* —
if it reads bare, it is a one-line change.

**5.2 Six of the seven habits change clump shape but not clump PLACEMENT.**
Only `rosette` decides which wood gets leaves. `distichous` flattens a clump and
leaves its orientation to the twig; it does not lay leaves into a plane held
level to the light, which is what actually makes a fir spray a spray. The
parameter's description is ahead of its mechanism, and that is the kind of gap
that reads as working until someone looks closely. Worth doing when foliage next
gets attention; not urgent, because the current version already reads as
different species.

**5.3 Decide an asset voxel budget.** An oak is 1.23M voxels at 5 cm, a sequoia
19M. Nobody has said what is affordable, and Phase 4 will care a great deal.
Cheap to answer once assets are actually streaming; guessing now would be
inventing a constraint.

**5.4 `hero-sequoia` shows 52% wood in its crown — deliberately not chased.**
Quadrupling the clumps moved it three points, because the visible wood is the
massive order-0 and order-1 limbs, which are 43% of the tree's limb length and
excluded from foliage by design. On a giant sequoia those limbs really are bare
bark. Recorded so it is not re-investigated.

---

## Deferred by owner decision

**Placement: an asset-forge panel, or `assetplacement.h`?** Deferred 2026-08-11.
No placement logic to be written until it is decided. One condition stands while
it waits: **neither side may grow its own copy of the other's numbers.** A
spacing authored in a panel and a spacing typed into an `AssetLayer` is this
project's recurring failure with a new subject, and a deferral makes it more
likely, not less, because both halves stay half-built.

---

## The order, and why

Phase 1 before Phase 2 only because three of its items are already diagnosed and
one of them (1.4) will resize the library — better to fix the known-wrong things
first, then build the net, then do the resize inside it. Phase 2 before 3 and 4
because both of those touch every asset at once and there is currently nothing
that would notice a regression. Phase 3 before 4 because an asset streamed into
the world with no appearance is not reviewable, and reviewing is the only way
Phase 4 gets verified. Phase 5 last because it is the only phase whose open
items need a person to look at a picture rather than a number.
