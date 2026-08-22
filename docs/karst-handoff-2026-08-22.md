# Karst handoff — session close 2026-08-22

Written for the next session picking up the karst (cave/tunnel) replacement.
Everything below is state, not plan; the plan lives in
`docs/karst-phase1-carve.md` and the approved programme in
`~/.claude/plans/tingly-hatching-blossom.md`.

---

## 1. Where the work physically is

| what | where | pushed? |
|---|---|---|
| **Phase 0** (Python prototype, both falsification checks, all imagery) | merged to `main` as **PR #230** | yes |
| **Phase 1** (the carve, the mirror, the amplifier wiring) | branch **`origin/karst/phase1-carve`**, tip **`14bb683`** | **yes** |
| this handoff doc | local branch `marcher/wave1-2-shading-and-tick-ceiling` only | no |

**Nothing is at risk of loss.** All four Phase 1 commits are on the remote.

### The branch is NOT karst-only — read this before opening a PR

`karst/phase1-carve` is 8 commits ahead of `main`, and **only 4 of them are karst**.
The other 4 belong to the concurrent ray-marching / detail-asset session, which
branched off the same line:

```
14bb683  karst Phase 1: wire the carve into the amplifier as a PROVEN no-op   <- karst
7aeb72a  karst Phase 1: the mirror, compiled against both targets             <- karst
0a24011  docs: Phase 5 steps 2-3 landed, plus three corrections               <- OTHER SESSION
d8bfe45  karst Phase 1: the carve, integer-only, with 14 tests                <- karst
6e1f1c0  docs: cover volume measured -- 26.5 MiB against a 500 MiB falsifier  <- OTHER SESSION
d4af5c0  voxel-core: cover compose, the byte-equality reference               <- OTHER SESSION
c417fab  docs: Phase 5 terrain quad retirement plan                           <- OTHER SESSION
1b790a1  karst Phase 1 opened: the carve, the removal, and the mirror         <- karst
```

Opening a PR from this branch as-is would ship the other session's Phase 5/6
planning docs and the cover-volume code under a karst title.

**The four karst commits touch a file set completely disjoint from both the other
session's commits and from the 16 commits `main` has gained since (all UE front-end
work) — verified, zero overlap.** So a clean branch cherry-picks without conflict:

```sh
# ONLY when the working tree is clean. It is not clean right now (see section 2).
git checkout -b karst/phase1-clean origin/main
git cherry-pick 1b790a1 d8bfe45 7aeb72a 14bb683
git push -u origin karst/phase1-clean
```

I did not run this myself: the checkout is shared with a live session whose
uncommitted work spans `worldgen.ush`, `VoxelGpuPoolComponent.cpp` and ~35 other
files, and a branch switch would have disturbed it.

---

## 2. State of the working tree at close

The checkout sits on **`marcher/wave1-2-shading-and-tick-ceiling`**, which has
**no upstream and three unpushed commits** (`719d2b2`, `9037b54`, `8d884de`) plus
~35 modified and ~38 untracked files. **All of that belongs to the ray-marching
session, not to karst.** Do not stage, commit, push or revert any of it.

The only karst-side untracked thing is **`bake-out/karst/`** (~18 MB): prototype
field and network `.npz` for tiles `-4_-4` and `-7_-5`, plus PNGs. It is
regenerable and should stay untracked. **The PNGs that matter are already tracked**
in `docs/images/karst/` — the map, sections, cross-sections, entrances, mouth
varieties and both centreline comparisons. Nothing in `bake-out/karst/` is a
deliverable.

I removed one stray zero-byte file named `=` at the repo root, left by a shell
mistake in my session on 2026-08-19.

---

## 3. What Phase 1 actually landed

Three new files plus a nine-line hook, all on `karst/phase1-carve`:

* **`voxel-core/include/voxelcore/karst.h`** — the carve. Integer-only (CI float
  ban), no square root. Per-column reduction to at most 8 segments, then
  `dz*dz < marginSq` per voxel, with a `minZMm`/`maxZMm` two-compare band early-out
  that the old depth-space caves could not do. Overflow past the cap is
  **reported** (`KarstColumn::overflow`), not silently truncated the way
  `kMaxCaveSegs`=12 is at `caves.h:450`.
* **`voxel-core/shaders/karst.ush`** — the bit-exact HLSL mirror. Its own file,
  deliberately, so it does not touch the 3,180-line contended `worldgen.ush` and
  so it can be compiled alone. It names its three hazards in the header: integer
  division direction (plain `/`, **not** `floorDiv` — that one is for lattice
  indexing), mandatory 64-bit intermediates (the product peaks near 2.1e16), and
  no sqrt on either side.
* **`voxel-core/shaders/KarstMirrorTest.usf`** — compile fixture. Passes both
  ADR-0001 targets: DXIL 6,560 B and SPIR-V 9,728 B via
  `dxc -T cs_6_0 -E KarstMirrorMain -O3` (plus `-spirv -fspv-target-env=vulkan1.1
  -fvk-b-shift 0 0 -fvk-t-shift 1 0 -fvk-u-shift 3 0`).
* **`voxel-core/tests/test_karst.cpp`** — 19 tests, registered in
  `voxel-core/tests/CMakeLists.txt`.
* **The hook**: `amplifier.h:15,126-143,326-329`, `amplifier.cpp:2575-2580`
  (column reduction) and `:2700-2703` (the carve, appended after `cavernCarveAt`).

### It is switched OFF, on purpose, and the proof is a PAIR of tests

`vxc_bench --radius 16 --digest` reads **`2b9df3f16281dfc0`** before and after,
on a bench binary rebuilt for the comparison. 751 tests pass, 0 fail. **Those
numbers are from commit time; the tree has since been modified by the other
session, so re-measure before quoting them.**

A no-op proof alone is worthless here — "it changed nothing" is exactly what a
feature that is never called looks like, and this repo shipped that failure once
(the standing-water veto reading an empty debug field). So read these two
together and keep them together:

* `karst_is_inert_in_the_amplifier_with_no_table`
* `karst_table_installed_actually_carves_through_materialAt` — installs a 5 m
  conduit 40 m under a **real** column and asserts `MAT_AIR` through
  `Amplifier::materialAt`, the shipped path, not `karstCarveAt` directly.

The golden to aim the mirror at is **`karst_golden_digest` = `0xA500EED45E8333B5`**
(`test_karst.cpp:322`).

**Why it is off:** the mirror is written and compiles but is *not yet included by
`worldgen.ush`*. Installing a real table now would make the CPU carve geometry the
GPU does not, and ADR-0006 makes that a silent desync vector.

---

## 4. What is blocked, and on what

Every remaining Phase 1 item is blocked on files the ray-marching session holds
uncommitted. Check `git status` before assuming any of these are still blocked.

1. **Include `karst.ush` from `worldgen.ush`** — one line, in the single most
   contended file in the repo (two sessions have already collided in it, one badly
   enough that the editor would not boot).
2. **The removal** (~2,600 lines): `caves.h`, `caverns.h`, `test_caves.cpp`,
   `test_caverns.cpp`, and the mirrored passes at `worldgen.ush:2292-2611`.
   Retire hash channels 20-25 and 30-31 in `hash_channel_registry.h` and **never
   reuse them** — that registry exists because a double-allocation once shipped.
3. **Two things that must be solved rather than deleted:**
   * `VoxelFootprintBand.h:174-220` derives the vertical streaming footprint by
     unrolling the cave carve's own bounds. Delete the caves and the underground
     band has no definition. **Note the Phase 0 interval-band probe came back
     NULL on today's world (0.2% saving) — so the band's payoff cannot be
     demonstrated until the new generator lands.** Do not build it on the
     strength of the original 35.4% figure; that number was fabricated by two
     probe bugs and is retracted.
   * `amplifier.cpp:1345-1392`'s `static_assert`ed 42.8 m / 91 m depth envelopes
     are what make the all-solid admission skip provable. A prevalent deep
     network breaks that proof by construction.
4. **`vxc_gpu` bit-exact parity** against `0xA500EED45E8333B5`, after the include.
5. **`kWorldGenVersion` 28 to 29** with a `core.h` changelog entry, mirrored as
   `VXC_WORLDGEN_VERSION_USH` in `worldgen.ush:159`. Kills goldens `cave_layer`
   and `cavern_layer`; moves `amplifier_deep_column_golden_digest`; respin seven
   prebuilt DXIL modules; re-pin `vxc_bench --digest` on gcc/clang/MSVC.

---

## 5. Carried from Phase 0 — not blocking, worth fixing

* **The spring criterion needs an *emergence* condition, not proximity-to-surface.**
  It only discriminates where the water table is deep: 29.7% agreement with
  `SECTION_HEADWATERS` on the alpine tile against 5.7% on the wet one.
  Highest-value field-stage fix.
* **Crouch-only passage is 6% of the network**, low for the crawl texture the wide
  radius distribution was chosen for. The lever is shifting the distribution
  **down**, not widening it further.
* **Radii are sized from junction degree** as a stand-in for discharge. The
  physical rule is r proportional to Q^0.4 and the bake already carries Q.

---

## 6. Phases beyond 1

**Phase 2 — bake the network as a sidecar.** `.vxkn` keyed by fine tile, cloning
the `.vxfl` artifact class at `cache.py:71,166-194`. This leaves
`product_identity_payload` and therefore `fine_provider_id` untouched, so the
existing 256 baked tiles stay valid and the ~60 CPU-hour world re-bake never
happens. Store the **un-subdivided** skeleton (~280 KB/tile) and apply the wander
at read time from the seed — subdivision costs 28x the segments. Cross-tile seams:
**own a system by the superblock containing its sink**, neighbours carry ghost
copies with the owner's values verbatim, so there is no recompute at the seam to
disagree about.

**Phase 3 — the engine underground.** Residency and the 64 m clamp, underground
LOD, the skirt/deep-box discontinuity, a water table, lighting. **This rides the
ray-marching session's P2-P4 and P7 rather than building against the quad path**,
which is saturated at 200M with 34,937 chunks refused today, before any cave
exists. Building underground LOD for a condemned representation is wasted work.

Phase 3 is larger than 1 and 2 together and is the only phase whose scope depends
on another session's schedule. **Phases 1 and 2 are both invisible by design** —
Phase 1 lands as a proven no-op, Phase 2 produces data nothing renders yet.
Nothing looks different in-game until Phase 3; intermediate confidence comes from
probes, not screenshots.

---

## 7. Traps that already cost this programme time

* **Rebuild every `vxc_*` before quoting any number.** Stale probes have faked
  results here four times.
* **`vxc_caveprobe`'s original 35.4% saving was fabricated** by two bugs: a
  footprint window top taken from MAX surface (sloping ground counted as air), and
  `chunksTouched` summed per-span, double-counting shared boundary chunks. The
  tell for the second was that the uncapped run reported *more* chunks than the
  capped one, which is impossible. Corrected result: **null**.
* **`FLOW_BIT_CHANNEL` is not a river mask.** It fires on 80% of a tile. Use
  accumulation thresholds.
* **`quant` in the tile header is a CODE, not a multiplier** (`{1:100, 2:250}`).
  Misreading it is a silent 100x error; the repo has already retracted one.
* **Sinuosity must be ADDED, not optimised for.** Every cost term multiplies by
  length, so shortest-path over a smooth field is near-straight — a 3x3 weight
  sweep moved tortuosity only 1.082 to 1.156 against a 1.3 target.
* **Vertical wander at 0.6x horizontal destroys walkability** (92.1% to 37.6%).
  Use `z_scale=0.10`. And the 92.1% itself was an artefact of averaging gradient
  over 217 m segments; the honest baseline figure is ~55%.
* **Do not create git worktrees in this checkout.** A prior worktree removal
  destroyed a cache through a Windows junction.
