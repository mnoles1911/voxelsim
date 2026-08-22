# Terrain GPU ray marching — plan

**Date:** 2026-08-19 (updated 01:30) - **Status: Phase 0 and Phase 1 COMPLETE AND MEASURED.
The marcher reads the real brick volume and passes its correctness gate. Shadows fixed. Phase 5 and
Phase 6 measured ahead of implementation. ONE unmeasured quantity now gates every headline number.**

| what | state |
|---|---|
| brick format contract | **PROVEN** - three independent implementations agree byte-for-byte |
| `voxel.GPU.VerifyBrickPack` | **PASS** - byte-identical to the CPU reference, every failure counter 0 |
| brick volume coverage | **99.4%** of chunks that reach a mesh job, plus ~9,300 all-solid chunks recovered from four pre-dispatch skip sites |
| cold-fill cost of that coverage | **none measurable** (35.4 s control vs 35.6 s, 5 s census resolution) |
| CPU pack cost | **0.163 ms/chunk**, 4.56x down, by reusing the mesher's own dense voxels |
| marcher reading the brick pool | **WORKS** - hits 8,015 tiles against the control's 8,016 |
| marcher correctness on bricks | **WITHIN REFERENCE NOISE** - 6x under the reference's disagreement with a one-pixel-displaced copy of itself, and indistinguishable from the stand-in source |
| cost of five indirections | **+0.943 ms, +18.3%** - over a 51.2 m box **with no skipping**. A per-step cost, not a verdict |
| identity check | **closes** - mode2 minus mode0 = 5.51 ms against 5.373 bracketed, residual 0.137 inside a 0.18 noise floor |
| Phase 5 (retire quad path) | **2.49x cheaper per chunk** measured on a real arm; built, gated, not enabled |
| Phase 6 (assets) | cover volume **10.5-26.5 MiB** measured, against the plan's 1,110 MiB estimate. The plan's coupling to finer voxels is **wrong** |
| shadows | **FIXED** - were never rendering; two causes in series. Cost **+15.83 ms (+83.8%)** |
| GI | **effectively free** (0.08 ms) on the shipping path. The +14.9% that governed it for a month describes a renderer nobody ships |
| automation | **15/15** |

**THE ONE NUMBER THAT GATES EVERYTHING: the skip ratio on this structure.** The marcher cannot beat the
raster path until rays skip empty space, and 9.19x was measured against a two-level mip over a flat
1-bit volume - a different structure - and has **never** been measured here. Every fps projection in
this document depends on it. B-2a exists to measure it on a 128 m ray at level 0, without waiting for
ring transitions.

**DEAD NUMBERS, do not re-inherit:** `march_ms = 0.13 + steps x 5.9 us` (measured on the flat volume);
the 9.19x skip ratio (different structure); `gpu-g0-sizing.md`'s "R0 is ~80% of resident chunks"
(measured 18.4% quads, 19.2% bricks, near-uniform); GI's "+14.9% p95 / x3.2 hitches".

**BASELINE DISCONTINUITY:** shadows started working on 2026-08-19 and are now on by default. `18.99 ms`
and `34.72 ms` are both real and describe different games. No A/B is invalidated - both arms of every
pair lacked shadows equally - but absolute frame times are not comparable across that line. Separately,
every perf leg on this project ran at **Low** scalability presets from a stale `GameUserSettings.ini`.

> ### Correction, 2026-08-19: GI is ON, and has been in every leg
>
> This plan and `docs/gpu-roadmap-remaining.md:101` both said voxel GI ships off at
> `voxel.GI.Enabled 0`. **It does not.** `voxel.GI.Enabled` (`VoxelGI.cpp:45`) and `voxel.GI.Volume`
> (`VoxelGIVolume.cpp:15`) have both defaulted to **1** since 2026-07-27. So **every baseline here,
> including the 18.99 ms A0 frame, already contains GI's cost**, and anyone budgeting against A0 while
> believing GI is off is double-counting.
>
> The **p95 +14.9% / hitches x3.2** that has governed this feature for a month is **RETIRED — measured
> 2026-08-20, no longer pending.** Three arms on the shadowed baseline: GI on **34.79**, GI off
> **34.71**, GI on with the solve disabled **34.90** ms p50. **G0 − G1 = 0.08 ms** inside a **0.19 ms**
> three-arm spread, and the middle arm sits *above* both ends — noise, not signal, with the solve
> counters reading exactly zero to prove the arm did what it says. Full record and the pre-registered
> decision rule in **§13**. It was taken on a configuration nobody ships (`voxel.Stream.GPU 0` component path,
> `voxel.GI.Volume 0`), its dominant term — the per-chunk vertex-colour re-shade — *structurally cannot
> occur* on the pooled path (`VoxelGI.cpp:3040-3049` only re-shades chunks with a `BrickComponents`
> entry, which pooled chunks never get), and it already failed to reproduce once at a cheaper camera.
>
> Separately and more importantly: **GI forces every level-0 chunk within 87.5 m off the D1
> direct-to-pool path onto the GPU readback path**, purely to retain quads for its own ingest
> (`VoxelWorldSubsystem.cpp:10389`, `VoxelGI.cpp:502`). D1 was a shipped win (PR #161); GI silently
> disables it in the near field, which is where the player is. This has never appeared in any GI
> measurement and is a cost to *streaming*, not to the frame.

**Anchor ADR:** `docs/adr/0008-flat-per-voxel-material-colour.md` (accepted, owner sign-off)
**Format contract:** `docs/brick-volume-format.md` · **Arm A data:** `docs/measurements/armA-drawpath-ceiling-2026-08-19.txt`
**Supersedes on the backlog:** §0.2 (HZB occlusion), §0.3 (S2-1 / S2-5)

## Phase 0 status board (live)

> **2026-08-21 (later, supersedes the banner below on two points).**
>
> **1. The disagreement bursts are ANTI-ALIASING DEPENDENT — falsifier closed, both arms.**
> Pre-registered: identical with AA off *and* varying with AA on; either half alone is not the result.
> Measured, same binary, same spawn, same 1,354,896 rays:
> AA off → three prints **byte-identical** hundreds of frames apart. AA on → **1.08× / 1.08× / 3.54×**.
> **But the mechanism I proposed is contradicted by the same pair.** I argued jitter only moves `TMax`,
> which can change *which* rays disagree and not *how many*. AA off produced **~90× more** disagreement
> (89,299 lost vs 277) and drove ill-posed rays to **99.8%**, making the arbiter unreadable.
> Working hypothesis, not a finding: with TSR off every ray leaves the **exact pixel centre**, and in an
> axis-aligned lattice that lands on voxel and brick boundaries — the degenerate grazing case the arbiter
> rejects. Jitter normally dithers rays *off* it. **So AA-off is not a clean control; it is a worst case.**
> *Never quote a disagreement number from an AA-off leg.*
>
> **2. GATE v4 delivered its first verdict at the cascade.** On the readable (AA-on) arm ill-posed fell to
> **20.2%**, under the pre-registered 25% guard, so the gate could finally speak: **PASS 1.08×, PASS 1.08×,
> FAIL 3.54×**. One FAIL in three is not a pass — but it is the first time this gate has said anything here.
>
> **3. Withdrawn:** "the flat arm violates its own segment 119× more than the hierarchy" came from the
> AA-off leg and does not survive. On the AA-on arm it is **7.6×** (afterTOut 174 vs 23). The *direction*
> stands and still makes "the hierarchy misses real content" unsafe to state — the reference is the dirtier
> of the two walks — but the 119× and the 239 km overshoot were degeneracy artefacts.
>
> **4. THE LARGEST UNRETIRED RISK IS NOW NAMED AND UNCHANGED FROM §Risks: mode 1 has never been shown to
> draw.** `voxel.March 2` (scratch) demonstrably runs every frame — `marchMs=3.124`, **19,712 of 21,340
> tiles**, `declined noView=0 noVolume=0 noTextures=0 unsupported=0 noPool=0`. But an A/B at a settled
> frozen pose with the quad path suppressed (`voxel.Stream.GPUCullDebugDrawNothing 3`) produced **no
> terrain at all** on the marcher arm — only the 30 km clipmap. Rings were on, so **reach is the cascade
> outer radius** (`VoxelMarchRenderer.cpp:3793`) and reach is *not* the explanation.
> The cheap discriminator, from `voxel.March`'s own help: run mode 1 with quads **not** suppressed —
> characteristic half-and-half depth noise means mode 1 *is* writing depth; an image identical to the quad
> control means it writes nothing.
>
> **5. Two instrument traps fixed, recorded because both fail toward "everything is fine":**
> `voxel-leg-summary.ps1` compared every leg against a 130-window bar that only a 2 s interval produces,
> while `voxel-run-gpu-arm.ps1` hardwires 5 s — so **every GPU leg read INCOMPLETE while having exited
> cleanly**. The bar is now derived from each leg's own command line. And `tasklist` through the agent's
> shell returns **nothing on this box even when an editor is running**; use `Get-Process`.


> **2026-08-21 — READ THIS BEFORE ANY RATIO BELOW.** The board's headline skip ratios were all measured
> at **51.2 m reach**. The 256 m ring work is in flight and has so far produced **no quotable ratio**:
> two defects made every ring number on the page meaningless, and both were instruments rather than
> renderer bugs.
>
> - **The ring comparator never walked the ring path** — `VoxelMarchTraverseRings` had exactly one
>   caller (the kernel), while the comparator called the non-ring walk directly. `hitL1 = 0` and
>   `ringDiscordant = 0` meant *never exercised*, not *broken*.
> - **The flat reference was clipped by the step budget, invisibly.** The two walks report exhaustion
>   through *different fields* (`bExhausted` vs `TermReason`); the counter read one and printed
>   **0.0%** while the reference was being truncated on nearly every ray. Budget 886 was sized for
>   51.2 m; a level-0 ray across 256 m needs 2,560+. **A clipped reference inverts the gate and
>   flatters the ratio at the same time**, which is why `39.88×` looked plausible.
>
> What IS established at 51.2 m: **37.09× / 36.86×** across two independent legs on an uncontaminated
> index, no budget clipping, gate abstaining rather than guessing. A direct collision counter proves
> **no chunk was ever shadowed** — an earlier claim that the whole archive was aliased is **withdrawn**;
> the guard's span statistic is a travel log that must fire in any long-enough leg.
>
> Gate v4 is now **two-sided** (`0.67×–1.5×`) and **abstains under 20 samples**. It failed the same
> configuration the one-sided bar had passed, on its first leg.
>
> The budget for 256 m is being **swept, not chosen** (886 / 2048 / 3328 / 4096), with the prediction
> registered before the run: the ratio must **plateau**, and the setting is the smallest budget on the
> plateau. If it never plateaus, the skip ratio is an artifact of the cap and the metric needs
> replacing rather than retuning.


> **One-line state:** the terrain draw is 81% of a settled frame (measured), the march costs
> `0.13 + steps × 5.9 µs` (measured), and a 4 km ray needs **4,480 steps without empty-space skipping** —
> at which the marcher is **slower than today**. Everything rests on the skip ratio. First measurement is
> **9.19× with two mip levels** (6.41× with one), pose risk retired — the camera was at surface + 5 m all
> along. That gives ~487 steps to 4 km → **6.56 ms frame, ~153 fps against today's 53, i.e. ~2.9×**.
>
> **Certification: consistent, not proven.** Against a construction-based upper bound on restart noise
> the verifier reads `WITHIN RESTART NOISE (not proof): 20 mismatch / 53 restartNoise`, and the 20 samples
> match the traced float mechanism — three-axis corner crossings on screen-adjacent rays. 20 of 1.35M
> cannot move a mean and step counts are bit-identical, so the *number* is unaffected. Recorded as open
> rather than claimed.

| gate | what | state |
|---|---|---|
| GATE 0-a | within-config spread | **DONE** — 0.18 ms (~1%) over four A0-equivalent legs |
| GATE 0-b | is resolution still free | **DONE** — yes for the raster (18.95 vs 18.99), **and yes for the marcher too** (4.96 vs 5.21 for 2.53× px). My "marcher gives up free resolution" claim was measured wrong |
| Arm A | bound the prize | **DONE** — 14.91 ms raster+submission, 0.53 ms walk+emit, 3.55 ms floor |
| shadows | is the baseline missing shadow cost | **DONE** — yes, and VSM is not the cause |
| Arm A-ring | does the cheaper heightfield project capture it instead | **DONE** — 6.94 / 11.25 / 16.02 / 18.99 ms. Cost tracks quads near-linearly. **Heightfield floor = 11.25 ms, so the march must beat 7.70 ms** |
| G1 | march spike — what replaces the 14.91 ms | **DONE** — 5.21 ms at budget 886 (51.2 m, dense, no skipping). Decomposed: memory 2.33 / ALU 2.75 / overhead 0.13. Cost model **0.13 + steps × 5.9 µs** |
| **G3** | **step count / skip ratio** — the single unknown the project reduces to | **9.19× (skip=2), 6.41× (skip=1)**, realistic pose (camera = surface + 5 m). flat 351.1 → skip=1 54.8 → skip=2 38.2. Of 53 disagreements, 33 are shared-face ties (excused, counted separately) and **the remaining 20 are at the reference's own float32 noise floor — proved, not assumed**: the flat walk disagrees with a 1e-6-nudged copy of *itself* 13× more often (157 vs 12 per 8M rays) than it disagrees with the skip walk. **Recorded as CONSISTENT, NOT PROVEN** — my earlier ''no residual defect'' was read off a mid-fill line and the converged verdict was DISAGREES. Against a construction-based upper bound it reads WITHIN RESTART NOISE (20 mismatch / 53 restartNoise). The verifier re-measures that floor every frame |
| G2 | `vxc_volumeprobe` brick census | **DONE** — 404 MiB at 10 cm; **1.74×** denser than quads on the same ground (not 3–14×); palette max 8, ≤4 bpp suffices; exponent 2.193 |
| Arm B | how much of the GPU frame is serial | **effectively answered.** The march adds to the frame ~1:1 (frame Δ tracked march Δ to within 0.4 ms across a 5 ms range), and terrain raster is 15.44 of 18.99 ms. Serial fraction is high; the original dial-sweep is redundant |
| **Arm A-loaded** | re-measure the prize at a dense pose | **BLOCKED, not skipped.** With assets the frame is **streaming-bound, not draw-bound**: 27.54 → 26.79 ms when the whole terrain draw is suppressed (0.75 ms, against 15.44 asset-free). Assets add +42% quads / +45% frame here. The temperate spawn needs fine tile (-8,-12) the current provider lacks. **The loaded-case prize is UNMEASURED, not measured-and-similar** — see §2d |
| Arm C | decompose the 3.55 ms floor | deprioritised — the floor is far smaller than the 9.5 ms that motivated it |

**Blockers cleared to get here:** the worldgen v28 `.ush` mirror skew (editor could not boot at all), a
stale `vxc_gpu` binary faking a determinism failure, and the standard leg spawn having no tile coverage
under the current bake.

---

## 1. Why now — and the reason is no longer frame rate

**The quad pool is out of address space. Today. In the shipping default.**

`VoxelWorldSubsystem.cpp:11650` carries the admission in its own comment:

> *"the first full-config attempt against the 80M default refused 28,205 chunks … i.e. **the SHIPPING
> default could not draw the world we now generate**, and every capture since has needed a
> `-VoxelPoolCapacityQuads` override."*

The pool was raised 80M → **192M quads on 2026-08-18** (1465 MB + 732 MB of chunk ids = **2,197 MB**).
It is still not enough. From `Saved/capture-VS_temperate.log`, timestamped `2026.08.19-02.10.38`:

```
loaded=54118  quads=200000000  residentQuads=200000000     <- the 200M clamp ceiling
34,937 x "voxel.Stream.GPU: no room for N quads ... Chunk left undrawn."
```

And there is a hard wall just above it: past **~268M quads the buffer's byte size crosses 2^31**, and
every int32/uint32 byte-offset in the lock and upload paths needs auditing before the clamp can move.

| pose | chunks | resident quads | quads/chunk |
|---|---|---|---|
| VS_tundra | 52,499 | 54.9M | 1,046 |
| VS_desert | 53,044 | 67.8M | 1,277 |
| VS_rainforest | 59,864 | 89.0M | 1,487 |
| BJ2_grassland | 49,481 | 131.0M | 2,647 |
| **VS_temperate** | **54,118** | **200.0M — SATURATED** | **3,696** |

**This is not a prediction, a projection, or a frame-time argument. It is last night's log.** Greedy
meshing has run out of room to represent the world worldgen already produces, one biome at a time, and
the next headroom step does not exist without an int32 audit.

That reorders the whole case. Ray marching was already the accepted direction (ADR-0008); it is now also
the only proposal on the table that makes the representation *smaller* instead of larger.

### The secondary reasons, still true

- **The frame is geometry-bound and the cheap levers are spent.** p50 15.2–15.4 ms at 2560×1440; GPU
  frame 18.4 ms; PrePass and BasePass each push 17,689,546 primitives across 7,131 draws into a
  **1552×873 internal** target — **~11 triangles per rendered pixel**. The cull retune already took
  over-draw 3.4× → 1.001×; the depth prepass is not removable. The backlog puts the remaining incremental
  ceiling at **~12–13 ms (~80 fps), not 10**.
- **42 authored species are stuck outside the world** (backlog §8), blocked on palette wiring into the
  vertex factory: `VertexColor` is full, `TexCoords[1]/[2]` are committed, so it needs `TexCoords[3]/[4]`
  plus an editor-bound `-run=pythonscript` material-graph regeneration. **A marcher deletes that problem**
  — it returns the exact voxel and face and calls `VoxelMaterialColor(mat, faceClass, voxel)` directly.

### Owner decisions

1. **GI:** voxel-native cone marching over the resident brick pyramid, plus Lumen *screen* traces for
   free. **No engine fork.**
2. **Spend the win on:** frame rate, finer voxels, loading/streaming. **Not** view distance — 4 km stays.
3. **v1 scope:** terrain **and** assets in one volume (full ADR-0008).

---

## 2. Honest magnitude

| axis | today (measured) | after (estimated) | factor |
|---|---|---|---|
| **Chunks refused, temperate pose** | **34,937** | **0** | **the real justification** |
| Resident geometry VRAM | 702 MiB of quads on the same ground *(pool is 2,197 MB, ~3× slack)* | **404 MiB measured** | **1.74× — see §2b** |
| Draw calls / frame | 14,291 | ~30 | ~475× |
| Primitives / frame | 35,380,670 | 0 | — |
| Voxelize work per chunk | 48×48×6 bricks | 32×32×4 | **3.375× less** |
| GPU frame | 18.4 ms | 8–11 ms | **1.7–2.3×** |
| ~~**Frame rate**~~ | ~~65 fps~~ | ~~~100 fps~~ | ~~~1.5×~~ **SUPERSEDED — see §2a** |
| Cold fill | 80–86 s | 25–40 s | ~2–3× |
| Resolution scaling | free (18.95 @900p vs 18.99 @1440p) | **still ~free** (4.96 vs 5.21 for 2.53× px) | **not a regression — measured** |

**Frame rate is the axis where 10× is not available** — but the ceiling in this table was argued from
`backlog.md` §0.1b (voxel pool = ~4.2 ms of a 13.7 ms render thread; the other ~9.5 ms being clipmap,
water, sky, base/post) and **§2a has since measured that floor at 3.55 ms, not 9.5.** The row above is
struck; §2a carries the live number. The July attribution is not wrong about July — it is a flight leg
at a different site in a world without assets — it simply does not transfer.

**Two claims retracted from the first draft of this plan:**

- ~~"the CPU quad shadow is a refund"~~ — **S2-5 already shipped.** The shadow is paged
  (`kShadowPageQuads = 1<<18`) and materialises only under CPU-authored writes. That win is taken.
- ~~"86× loading headroom to 92,000 chunks/s"~~ — **the producer was never the limiter.** The apply
  census measured **265.7 chunks/s on the GPU arm against 261.1 on the CPU arm** — a 1.8% difference
  against a 1.2% within-config spread ("MESHER INDEPENDENCE HOLDS"). And `--radius 128` was one-shot,
  level-0-only, with no tile I/O, no admission policy, no edit overlay and no asset resolution. Removing
  apply cost removes **one** serial stage of several. Expect a large multiple; **quote no number until
  measured.** Predicted next limiter: `VoxelFineTileStreamer`'s block-until-ready rule over a 22 GB cache.

---

## 2a. MEASURED 2026-08-19 — Arm A ran, and the gate passes by 5x

Full record: `docs/measurements/armA-drawpath-ceiling-2026-08-19.txt`. Static settled pose at
`-VoxelSpawnAt=-61440,-61440` (metres), 2560×1440, sun pinned.

| arm | p50 | p95 | frames |
|---|---|---|---|
| **A0** shipped defaults | **18.99 ms** | 21.52 ms | 11,465 |
| **A3** mask 3 — submit nothing, walk + emit still run | **4.08 ms** | 5.03 ms | 52,140 |
| **A3** mask 7 — pool does nothing at all | **3.55 ms** | 4.85 ms | 59,678 |

**The whole frame, decomposed:**

| component | ms | share |
|---|---|---|
| GPU raster + submission (`A0 − mask3`) | **14.91** | 78.5% — *what a marcher replaces* |
| render-thread cull walk + range emit (`mask3 − mask7`) | **0.53** | 2.8% — *what a marcher deletes* |
| everything else — clipmap, water, sky, base/post (`mask7`) | **3.55** | 18.7% — *the floor* |
| **terrain prize** | **15.44** | **81.3%** |

The kill criterion was `A0 − A3 < 3 ms`; "proceed unconditionally" was `> 8 ms`. **Measured 14.91 ms.**

A0's GPU capture: **17.514 ms, 12,138 draws, 43,681,258 primitives** —
`PrePass 7.703` + `BasePass 7.542` = **15.245 ms, 87.0% of the GPU frame**. `WorldTick` (meshing) is
**0.042 ms**, because this is a settled static pose, so this arm isolates draw cost almost perfectly.

**Two corrections to §2 of this plan, both in the project's favour:**

1. **The residual floor is 3.55 ms, not ~9.5 ms.** §2 argued from `backlog.md` §0.1b that the render
   thread would become the bound near 9.5 ms and cap the result around 100 fps. Measured, everything
   that is not the terrain draw costs **3.55 ms**. The clipmap/water/sky/post block is far cheaper at
   this site than the July attribution implied.

   **So §2's ~1.5× frame-rate estimate is wrong and too pessimistic.** The arithmetic now is
   `3.55 ms + march`: a 7 ms march gives ~95 fps, a 2–4 ms march gives **133–180 fps, i.e. 2.5–3.4×**.
   The number is still not final, because the march has not been measured — that is G1, in flight.

   Note also the render-thread pool cost here is **0.53 ms**, not July's ~4.2 ms (walk 1.0 + emit 3.2).
   Different site, static pose, 50,485 chunks and 5,571 ranges. Do not carry the 4.2 ms figure forward.
2. **Meshing is not the frame when nothing is streaming.** 0.042 ms here against July's 21–28%. The July
   figure came from a flight leg with terrain arriving; both are true, of different conditions.

**GATE 0-b answered: resolution is still free.** 1600×900 → 18.95 ms against 2560×1440 → 18.99 ms —
2.5× the internal pixels for 0.2%. Every 900p-derived conclusion in `docs/measurements/` therefore
survives, which was genuinely in doubt after assets and the biome rebalance.

I then wrote that this was **the marcher's one real regression** — that the property exists only because
the frame is geometry-bound, and marching is O(pixels) by construction. **Measured, that is wrong.** The
spike at budget 886, both arms converged: **4.96 ms at 900p against 5.21 ms at 1440p — 2.53× the pixels
for 5%.** The march is *latency*-bound, not throughput-bound: each DDA step's address depends on the
previous step's result, so waves spend most of their life on dependent loads, and at 0.54M pixels the GPU
is nowhere near saturated. Consistent with the ALU/memory split — neither half is near a hardware limit.

So the marcher does **not** obviously give up free resolution, `march_ms ≈ 0.13 + steps × 5.9 µs` is a
**per-frame** model at ~1.3M internal pixels and must not be scaled by pixel count, and the ~61% internal
render is load-bearing only for TSR stability — which is all `DefaultEngine.ini:70-73` ever claimed. It
will stop being free somewhere; 4K internal is 6× 1440p and unmeasured. **The proved range is 0.54M →
1.36M internal pixels.**

**The shadow question is now answered, and VSM was not it.** A leg with `r.Shadow.Virtual.Enable 0`
measured **18.96 ms** — indistinguishable from the 18.99 control — with an unchanged GPU split and **no
`ShadowDepths` pass in either capture.** The accumulating census reports `shadowGathers=0 MEASURED (not
absent)` cumulatively over **3,840 camera gathers**, sun at +38°. So terrain contributes no shadow-cascade
draws by some mechanism other than VSM caching, still unidentified.

*(A trap worth carrying: the per-line `shadowGather=%d` field in the `cull:` log is a **boolean for the
one sampled gather**, printed every 601st gather — not a count. The census is the evidence, and it says
so in words. Do not quote the per-line field.)*

**This may be a visual defect, not just a perf curiosity, and it is worth someone looking at a screenshot.**
Everything that should make terrain cast is configured to: the component sets `CastShadow = true`
(`VoxelGpuPoolComponent.cpp:2387`), every batch inherits it (`:1249`), the proxy reports
`bShadowRelevance`, and the sun's `SetCastShadows(true)` is applied at init and never flipped (no
`VoxelSky sun shadows` transition line appears in any leg). Yet the accumulating census reports
**zero shadow gathers over 3,840 camera gathers**, no `ShadowDepths` pass appears in either GPU capture,
and disabling VSM changes neither. **If terrain genuinely casts no dynamic shadow, the world is missing
its own self-shadowing** — which would read as flat relief on slopes and no contact darkening under
overhangs. I could not identify the mechanism and did not chase it further; it is orthogonal to this
plan but should not be lost.

**Consequence: 18.99 ms and July's 18.4 ms are both best cases.** Neither contains terrain shadow cost.
Switch shadows on and the raster path pays up to four more cascade passes over ~44M primitives while a
marcher pays one wave-uniform secondary ray — so every number here **understates** the marcher's
advantage. Quote the speedup twice and say which. Whether the absence is itself a visual bug is flagged,
not chased.

**Within-config spread (GATE 0-a) is 0.18 ms (~1%)** across four A0-equivalent legs (18.99 / 18.95 /
18.96 / 18.81). The Arm A delta is ~80× that.

### The competing architecture, tested rather than dismissed

July's session summary proposed a **much cheaper** route to the same prize: *"drawing distant rings
(L3–L5, 53% of pool quads, 512 m to 4 km, sub-pixel voxels) as heightfield rather than voxel geometry.
That is a feature, not an optimisation."* If most of the 14.91 ms lives in the outer rings, that project
captures most of the win for a fraction of a marcher's cost, and this plan should not be started.

So it gets measured, not argued: `-VoxelMaxRingLevel=` 0 / 2 / 4 against the 6-ring control, same site
and pose. (`tools/voxel-run-gpu-arm.ps1` gained `-ExtraArgs` for this — ring composition is read before
the first `RecomputeDesiredSet`, so it cannot be a cvar and could not previously be swept.)

- **Cost concentrated in outer rings** → the heightfield project is the better first move, and this plan
  is deferred again.
- **Cost concentrated in R0** → only a marcher helps, because R0 is where true 10 cm voxels are and a
  heightfield cannot represent them.

**Result — the competing architecture is real, and it sets the marcher's bar.**

| arm | rings | quads | p50 | draw cost | draw share |
|---|---|---|---|---|---|
| ring0 | L0 | 18.4% | 6.94 ms | 3.39 | 22.0% |
| ring2 | L0–L2 | 52.3% | **11.25 ms** | 7.70 | 49.9% |
| ring4 | L0–L4 | 78.1% | 16.02 ms | 12.47 | 80.8% |
| A0 | L0–L5 | 100% | 18.99 ms | 15.44 | 100% |

Draw cost tracks quad count almost exactly, so **no ring is disproportionately expensive and there is no
cheap subset to attack.** `ring2` *is* July's proposal measured — true voxels to 512 m, heightfield
beyond — and its **floor is 11.25 ms (89 fps)**; floor, because this arm draws nothing out there at all
and a real heightfield is not free.

| option | frame | cost |
|---|---|---|
| do nothing | 18.99 ms (53 fps) | — |
| heightfield distant rings | **≥ 11.25 ms (≤ 89 fps), 1.69×** | weeks |
| ray marcher | **3.55 ms + march** | months |

⇒ **The marcher must land the march under 7.70 ms merely to tie the cheaper project**, and well under it
to justify the difference. That is now the spike's real bar.

What the heightfield does *not* do, and which should weigh against its 1.69×: it does not fix the pool
saturation (34,937 chunks refused today), does not unblock the 42 authored species, does not enable finer
voxels anywhere, and permanently gives up true voxel geometry beyond 512 m.

**Still owed before Phase 1:** G1 (the march spike — what replaces the 14.91 ms) and G2 (the brick
census), both in flight. Mask 7's first attempt voided (abrupt self-exit at 51 s, no fatal error, no
crash dump); it re-ran unchanged and clean, and `RecordGather` is self-contained, so that was flakiness
rather than the bit-4 path.

---

## 2b. MEASURED — the VRAM refund is 1.74×, not 3–14×

`vxc_volumeprobe` walked the full six-ring cascade at the measurement site on real tiles, real climate and
438 asset species, no sampling. Record: `docs/measurements/brick-census-2026-08-19.txt`.

**Brick vs quad off ONE walk, same ground: 404 MiB of bricks against 702 MiB of quads — a 1.74× saving.**

An earlier draft claimed 3–14×. That was wrong, and the reason matters: it compared the brick estimate
against the **2,197 MB shipped pool**, which is ~**3× the cascade's own quad content**. Most of the
apparent refund was pool saturation and allocator slack, not the representation. Both facts are real, but
they are separate arguments and must not be added:

- *the format* is **1.74×** denser than greedy quads on identical ground;
- *the pool* separately carries ~3× slack and is refusing 34,937 chunks today.

The 4× estimate spread had a mundane cause: **"mixed brick" has two legitimate definitions.** By volume
(12.8 m below the shell) 30.3% of slots are mixed; over the surface shell the mesher actually meshes,
90.1% are — but there are 3.8× fewer of them. A census reporting one without naming which recreates the
spread. The probe now names it, and `--depth-m` is an explicit input.

**Palette settles bit-width:** over 916,901 mixed bricks, mean 3.73 materials, **max observed 8**. Nothing
in the cascade needs more than 4 bpp and 78% needs ≤2; adaptive lands at 2.01 bpp (R0) → 2.63 (R5). The
5–8 bucket is entirely assets — terrain-only bricks max out at 4.

**Three findings that change the design, not just the numbers:**

1. **The flat brick index is already 48% of the 10 cm total** (193 of 404 MiB), and at 2.5 cm it is
   12,080 MiB — 30× an entire 10 cm world. §4 defers the sparse-hash/octree decision to 2.5 cm.
   **It should be taken at 10 cm.**
2. **`tryCollapse` is worth ~250 MiB of the 404** — 62%. 67.1% of slots collapse to an 8 B descriptor.
3. **5 cm is a worldgen change, not a lattice change.** `Amplifier::column` is addressed in integer
   *level-0* voxels — there is no sub-100 mm horizontal query — so a finer sub-lattice is exact in z and
   **saturated in xy**. Halving the voxel size does not by itself buy horizontal detail. This directly
   qualifies the owner's "finer voxels" spend and is picked up in §4.

The scaling exponent is **2.193, not 2.0**, so every finer-voxel row is ~16% worse per halving than this
plan assumed. 2.5 cm across 4 km measures **16.5 GiB** — the strike stands.

---

## 2c. PHASE 0 VERDICT — it cannot settle the frame-time question, and that is the finding

Phase 0 was designed to answer "is the terrain draw enough of the frame to justify replacing it". It
answered that emphatically: **yes, 15.44 ms of 18.99.** But the ring ablation introduced a second question
the gate was never built to answer, and that question now dominates:

> **Is a marcher better than the far cheaper heightfield project?**

The heightfield floor is 11.25 ms, so the march must come in under **7.70 ms**. The spike measures
**5.21 ms** — but for **25.6 m of reach, in a dense single-level volume with no empty-space skipping, no
mip pyramid and no sparse indirection.** The real marcher must cover **4 km**, which is 156× the range,
and everything that makes that affordable is exactly what the spike does not have.

**The spike has since produced a cost model, and it narrows the whole question to one unknown.**
Decomposing the 886-step arm: memory ("asking the volume") **2.33 ms / 45%**, the DDA loop itself
**2.75 ms / 53%**, pass overhead 0.13 ms. Both halves scale with step count — the no-fetch permutation
still walks all 886 steps — so at 1552×873 internal:

> **march_ms ≈ 0.13 + steps × 5.9 µs**  *(of which 2.6 µs is the fetch)*

The spike's 886 steps buy 51.2 m with **no skipping**. So the marcher's viability reduces to:
**how many steps does a pyramid-accelerated 4 km march take?**

A horizontal ray needs **4,480 steps** to reach 4 km *with the ring cascade's coarsening already
included* (1280 in R0 at 10 cm, then 640 per ring as voxel size doubles). Fed through the measured model
and added to the measured 3.55 ms floor:

| skip ratio | steps | march | frame | fps | vs today |
|---|---|---|---|---|---|
| **1× (none)** | 4,480 | 26.56 ms | 30.11 ms | **33** | **worse than today** |
| 3× | 1,493 | 8.94 ms | 12.49 ms | 80 | 1.5× |
| 5× | 896 | 5.42 ms | 8.97 ms | 112 | 2.1× |
| 10× | 448 | 2.77 ms | 6.32 ms | 158 | 3.0× |
| 20× | 224 | 1.45 ms | 5.00 ms | 200 | 3.8× |

**Without empty-space skipping the marcher is slower than the renderer it replaces.** Every optimistic
number in this plan is really a bet on the skip ratio.

**Measured 2026-08-19 (provisional).** A coarse mip over the existing 512³ volume, hierarchical march,
exact step histogram:

| arm | overall | hits | misses | p95 | mismatch/ties | ratio |
|---|---|---|---|---|---|---|
| flat control | 351.1 | 221.2 | 442.4 | 605 | — | — |
| skip = 1 level | 54.8 | 52.4 | 56.5 | 86 | 20 / 33 | **6.41×** |
| **skip = 2 levels** | **38.2** | 57.0 | **25.0** | 94 | 22 / 38 | **9.19×** |

| config | steps to 4 km | march | frame | fps | vs today |
|---|---|---|---|---|---|
| skip = 1 | 699 | 4.26 ms | 7.81 ms | 128 | 2.43× |
| **skip = 2** | **487** | **3.01 ms** | **6.56 ms** | **153** | **2.90×** |

⇒ **~2.9× — about 153 fps against today's 53.** Note skip=2 measured 48.8 on the *pre-fix* binary; the
true-A&W correctness fix improved it to 38.2, so the fix made the fast arm faster. The old walk was not
merely wrong on 52 rays, it was taking unnecessary steps everywhere.

**A pose risk I raised twice, and then retired by measurement — it was my error.** I believed these
numbers came from an elevated camera (I read `depth=60m` in the run header as camera height; it is a
different parameter), which would flatter the ratio because skipping helps misses (7.8×) far more than
hits (4.6×). Three switches later — `-VoxelPerfDepth`, `-VoxelSpawnAltM`, and a new `-VoxelPerfHeightM`
I added — the census stayed byte-identical every time, because **a static flight never leaves the spawn
pose**; those switches move the *path centre*, which a pinned pose does not use.

The arithmetic settles it: path centre at +30 m = 221758 and at +2 m = 218958 ⇒ **surface = 218758**;
spawn pose z = 219258 ⇒ **camera = surface + 5 m**. **Every skip number was already measured from a
realistic player-height camera**, and 58.7% misses is just a horizontal view with sky in half the frame.
**6.41× stands without a pose caveat.**

**The last correctness question is NOT closed, and the way it is being chased is the most reusable thing
here.** The 20 residual mismatches may be the *reference implementation's own float32 noise*:
a ray crossing two planes within ~2 ULP, where the flat walk's accumulated `tMax` and the sub-walk's
freshly computed one order them differently. No cell is skipped. The control that settles it — flat
against a 1e-6-nudged copy of itself — disagrees **157 times per 8M rays**, against **12** for flat vs
skip. *The reference is 13× less self-consistent than the thing it was judging.*

So the verifier now measures that floor every frame (a second flat walk over a segment scaled by 1−1e-7)
and grades `AGREES` → `AT REFERENCE NOISE FLOOR` → `DISAGREES`. **Calibration against a control drawn
from the same population, not a loosened threshold** — the floor cannot be tuned to hide a defect.

**But converged, the two measurements disagree, and the question is open.** In-engine the verdict is
`DISAGREES — 20 mismatch / 7 refNoise`: self-noise measures *lower* than the skip disagreement, the
opposite ordering to the offline model's 157-vs-12. Either the in-engine control is a weaker perturbation
(it scales the segment; the offline one nudged the entry point, which is where the accumulated-`tMax`
divergence originates) and understates the floor, or ~13 rays are a genuine defect. **The ratio is
unaffected either way** — 20 rays of 1.35M cannot move a mean, and the step counts are bit-identical —
**but 6.41× / 9.19× remain UNCERTIFIED**, and the instrument's refusal is being respected rather than
overruled.

**A design finding worth more than the ratio.** skip=2 crushes misses (56.5 → 25.0) and costs hits
(52.4 → 57.0) with a worse p95 (86 → 94). Since misses are 58.7% of rays it wins on the mean — but the
trade is real, and a fixed hierarchy depth serves the two ray populations badly at once.

I suggested adaptive depth would win *both* mean and tail. **That was an assertion and it does not
survive contact.** An offline model — validated first against the three known engine numbers, and found
to reproduce the flat/skip ratio to ~10% and the skip=2 tail direction, but **not** the skip=2 mean
ordering — indicates a first-cut adaptive scheme wins the mean (−16%) and **loses p95** (118 vs 109),
because the tail is dominated by descent overhead on rays near surfaces, which adaptive adds to rather
than removes. **If adaptive is pursued, read p95, not mean.** The mean is the easy half; the tail is
where a 4 km budget is spent.

*And a caveat on the extrapolation above:* `0.13 + steps × 5.9 µs` was calibrated at the flat step
distribution and applied to skipped ones whose shape differs (mean falls 9.2×, p95 only 7×). On SIMD a
wave costs its slowest lane, so a relatively heavier tail costs more per mean-step than the calibration
assumes. **The 2.9× is mildly optimistic in a way the arithmetic does not show** — bounded at roughly
1.3× at worst, but real.

*Both directions of error:* 5.9 µs/step came from a dense uniform 10 cm volume, and coarse rings have a
far smaller working set per unit distance, so µs/step at range is probably lower — the table is
pessimistic there. And a horizontal 4 km ray is the worst case: the flat census says 58.7% of rays miss
and hits terminate at ~221 steps, so the *mean* ray is much cheaper. Against that, the table ignores ring
crossings. The census's independently measured **91–96% skippable** is where a 10–20× ratio would come
from.

The census says **91–96% of the flat index is skippable** and a brick column holds 19.0 bricks at R0
falling to 2.0 at R5. **Nobody has the step count**, and the spike cannot produce it — but a step-count
histogram over a real brick pyramid is a P1/P2 deliverable, not a months-long commitment. **That is the
cheapest thing that would settle this project, and it did not exist as an item before today.**

**And the 7.70 ms bar is itself site-specific — it does not generalise.** It was derived from `ring2` at
this plan's measurement site, where L0–L2 is 52.3% of quads. At the saturating temperate pose L0–L2 is
**98.8%**, so a heightfield over L3–L5 there removes ~1% of the geometry and saves ~1% of the draw. The
heightfield project is worth 1.69× where the *far* field dominates (tundra, alpine, desert) and close to
**nothing** where the *near* field does (temperate forest with composed assets) — which is precisely the
case that saturates the pool and produces the worst frames. A marcher helps in both.

⇒ **The correct reading of the bar is a range, not a number: between ~7.7 ms (far-field-dominated scenes)
and ~18 ms (near-field-dominated ones).** The spike's 5.21 ms clears it comfortably in the case that
actually hurts, and marginally in the case that does not.

**Even so, the spike cannot decide this, by construction.** The step-budget curve says the cost is dominated by
the minority of rays that travel far, and the census says 91–96% of the flat index is skippable — together
a strong *suggestion* that a pyramid attacks precisely the expensive population. It remains a suggestion.
The standing rule on this project is that a predicted saving does not go on a plan until an experiment
removes the work and measures it, and seven predictions were falsified 7/7 the last time that rule was
tested. **What would settle it is a spike with a real brick pyramid over real terrain at cascade range —
which is most of P1 and P2.**

### The heightfield alternative does NOT fix the saturation — checked, and I had this wrong

I first wrote that it does, reasoning that L3–L5 is 47.7% of resident quads so dropping them would take
the saturating pose from 200M to ~105M and fit the 192M pool. **That is wrong, and the per-pose data says
so.** Ring composition is strongly biome-dependent:

| pose | L0 | L1 | L2 | L3 | L4 | L5 | total |
|---|---|---|---|---|---|---|---|
| VS_tundra | 15.8% | 12.9% | 17.5% | 18.9% | 19.2% | 15.7% | 54.9M |
| *this plan's measurement site* | 18.4% | 15.1% | 18.8% | 17.6% | 15.6% | 14.5% | 46.8M |
| VS_desert | 31.4% | 18.7% | 15.4% | 14.3% | 11.6% | 8.6% | 67.8M |
| VS_rainforest | 32.4% | 19.1% | 15.2% | 14.1% | 10.8% | 8.4% | 89.0M |
| **VS_temperate** | **51.6%** | **36.2%** | 11.0% | 1.1% | 0.04% | 0.02% | **200M — SATURATED** |

At the pose that actually saturates, **L3–L5 is 1.2% of the pool**, not 47.7%. Dropping it frees ~2.4M of
200M quads. The heightfield project would not move the saturation at all.

And the reason is itself the point: the refusals are logged **"no room … at level 5"**. L0+L1 consumed
**175M of the 200M pool** and the outer rings were *starved* — so temperate's flat-looking outer rings are
a **symptom** of saturation, not evidence about where geometry lives. **The saturation is a NEAR-FIELD
problem**, driven by dense vegetation and composed assets within 256 m, and the heightfield project
addresses the far field. Only something that changes the near-field representation touches it.

**Two caveats this forces on every frame number in this plan, and the second is the bigger.**

1. **Site.** Today's legs ran at a site carrying **46.8M quads against temperate's 200M+** — a quarter of
   the worst case, with a flat ring mix where the worst case is 88% near-field.
2. **No assets.** Every leg ran **without `-VoxelAssetDir`**; all four biome captures ran *with* it.
   Assets are shipping content composed into terrain chunks, and in a dense biome they are most of the
   geometry — tundra-with-assets is 54.9M (close to this site's asset-free 46.8M, because tundra is bare)
   while temperate-with-assets is 200M.

So **14.91 ms, 3.55 ms and 11.25 ms are all measured on a light, asset-free scene and must not be quoted
as the worst case.** Re-measuring Arm A at a temperate-class pose *with* assets is the highest-value
outstanding leg. It is runnable — the captures reached it coarse-only with
`-VoxelPoolCapacityQuads=200000000`, and even then refused 34,937 chunks.

What remains genuinely exclusive to the marcher:

| | |
|---|---|
| **fixes the near-field pool saturation** | the heightfield does not — L0+L1 is 87.8% of the saturating pose |
| per-voxel colour without vertex-factory plumbing | unblocks the 42 authored species (backlog §8) |
| finer voxels | real, but qualified — 2× in z, saturated in xy without a worldgen change |
| true voxel geometry beyond 512 m | a look decision, not a perf one |
| occlusion culling for free | supersedes backlog §0.2 |
| deletes mesh → pool → cull → submit entirely | maintenance, and the 0.53 ms render-thread cost |

Against: **frame-time parity at 4 km is unproven**, and it is
months against weeks.

⇒ **Where this lands.** The heightfield is a genuine competitor only in far-field-dominated scenes, and
it does not touch the saturation that is breaking the renderer today. The marcher addresses both, at the
cost of an unproven 4 km march and losing free resolution. The remaining honest options are **(a)** invest
P1+P2 specifically to measure a pyramid-backed march at cascade range before committing to the rest — the
only thing that converts the frame-time argument from prediction to measurement; or **(b)** commit on the
saturation and asset-unblock grounds, with frame-time parity stated as a risk rather than a benefit.
Building the heightfield first is no longer attractive: it costs weeks and leaves the actual failure
untouched. **This is an owner decision and I am not making it.**

---

## 2d. The asset-loaded frame is bound by something else entirely

Re-running Arm A at the same site **with `-VoxelAssetDir`**, both arms settled to the same pool:

| arm | p50 | p95 | quads |
|---|---|---|---|
| asset-A0 | 27.54 ms | 55.76 ms | 66,394,419 |
| asset-mask7 — terrain **not drawn** | **26.79 ms** | 59.69 ms | 66,372,475 |

**Suppressing the entire terrain draw is worth 0.75 ms with assets on, against 15.44 ms without.**

That is not evidence against the marcher. It is this project's own documented shape: *work removed from a
subsystem does not become frame time when something else is the binding constraint.* The asset legs never
stop streaming — the pool grows to leg end and p95 blows out to 56–60 ms against 21 ms asset-free — so the
frame is bound by asset streaming and composition on the CPU, and the GPU draw hides underneath it.

**Three things follow, and they matter more than the number:**

1. **The terrain draw dominates only once the world has settled.** While streaming — which is most of the
   time a player is moving — the binding constraint is elsewhere. Both are true of different moments, and
   the 15.44 ms prize is a *settled-world* figure.
2. **The asset-loaded prize remains unmeasured**, and cannot be measured until asset streaming settles.
3. **The asset streaming regression is the more urgent of the two problems.** It is what is costing frames
   right now: 27.5 ms p50 / 56 ms p95 with assets against 19.0 / 21.5 without. The marcher does not fix
   it — §9 already says two of its four causes are untouched — and the `AssetBankLibrary::bankGrid` mutex
   is a separate, smaller, and more urgent project.

Do not quote 0.75 ms against the marcher, and do not quote 15.44 ms as the loaded-case prize. Neither is
a measurement of the other's condition.

---

## 2e. P2's VRAM gate is 223 MiB, not 415 — the pool improved on the census

The census priced the cascade at ~415 MiB **including a 193.3 MiB dense flat brick index**. `FVoxelBrickPool`
does not allocate one: it addresses through a **32 B record per resident chunk (~1.4 MiB)** — which is
exactly what census finding (b) said was worth doing at 10 cm rather than deferring to 2.5 cm.

| | MiB |
|---|---|
| cells | 128.5 |
| occupancy | 56.0 |
| descriptors | 23.1 |
| palette (fixed 16 B) | 14.0 |
| chunk records | 1.4 |
| **resident total** | **223.0** |
| *(census figure, with dense index)* | *415* |

**So the ±20% gate must be held to 223 MiB (178–268), not 415.** Held to 415 this design measures 46%
low and fails a gate *for being cheaper than a structure it does not build* — the kind of false negative
that gets a good design reverted.

Committed VRAM at shipped defaults is 314 MiB (~1.4× the resident set as churn headroom). And a limit
worth quoting alongside any number: **nothing frees from this pool yet**, because nothing draws from it.
It bounds itself by evicting in insertion order rather than failing, so `allocFail = 0` stays meaningful —
but **resident bytes are an upper bound on the live set only while `GetEvictions()` is 0**, which
`voxel.Brick.Stats` warns about. Quote both or neither.

---

## 3. Phase 0 — the gate, and it can still kill this

The standing rule (**7/7 predictions falsified in one day**) governs: no saving goes on the plan until an
experiment removes that work and measures the frame. Ordered cheapest-kill-first.

| | what | cost | what it kills |
|---|---|---|---|
| **G1** | the march spike | 1 day, no producer work | the whole project, cheapest |
| **G2** | `vxc_volumeprobe` census + saturation replication | ½ day, no editor | the "finer voxels" scope |
| **G3** | GATE 0 + Arm A | 1 day + 1 afternoon of code | the frame-rate case |
| **G4** | Arm C | 1 day + 5 lines | **reorders the project** |
| **G5** | Arm B | 1 day | the last predictions |

**GATE 0 — is resolution still free?** One leg at 1600×900 against one at 2560×1440. In 2026-07 it was
free (30.61 vs 30.59 ms), and the session summary attached a standing warning to re-check it. The world
has since gained assets, amplified relief and 2.5× the quad density. **If resolution is no longer free,
every 900p-derived conclusion in `docs/measurements/` is void and this gate re-bases before Arm A means
anything.** Plus three shipped-default replicates for within-config spread — no delta below it is a result.

**Arm A — bound the prize.**

| arm | how | removes |
|---|---|---|
| A0 | shipped defaults | — |
| A1 | `-VoxelMaxRingLevel=0` | R1–R5 (~43% of chunks) |
| **A3** | **`voxel.Stream.GPUCullDebugDrawNothing 1` (new, ~5 lines)** | **the terrain draw, entirely** |
| A4 | A3 + `voxel.Water.GPU 0` | terrain draw and the water pool |

A3 is the single most valuable measurement here and costs one afternoon: an early-out in
`GetDynamicMeshElements`, gated alongside the existing `GDebugAllVisible` / `GDebugSplit` /
`GDebugInvert` family (`VoxelGpuPoolComponent.cpp:380-412`) — that family exists precisely to separate
hypotheses with identical symptoms. It keeps every CPU-side cost and submits nothing.

*Arms that do not do what they look like:* `GPUCullDebugAllVisible` **adds** geometry (it skips the
frustum test); `GPUCullDebugInvert` draws the rejected 95%; `voxel.Stream.GPU 0` swaps renderers rather
than removing geometry.

**Arm B — how much of the GPU frame is serial?** Answer by regression against a dial, not by
instrumentation (timestamps include overlap; `ProfileGPU` serialises what it measures). Four points each
on `MeshBatchCap`, `-VoxelMaxRingLevel`, and — the cheap decisive one — `r.ScreenPercentage`
71/100/141/200, which varies pure pixel work with zero geometry confound. Output: **Δframe-ms per
Δ GPU-pass-ms**, the number missing since 2026-07-28.

**Arm C — where is the unattributed ~9.5 ms?** `-VoxelClipmap=0` (new, ~5 lines), `voxel.Water.GPU 0`,
**`-VoxelGIOff`** (a new command-line switch — **NOT** `voxel.GI.Enabled 0` via `-ExecCmds`, which lands
after streaming has begun and measures a mixed state; `VoxelGI.cpp:3085` records that this exact trap
already cost three runs), post/TSR off, and all together. **Blunt consequence: if the 9.5 ms is mostly the
clipmap, the correct first project is deleting the clipmap, and this plan's phasing reorders.** That
should not be discovered in month four.

**Go / no-go, stated before the data:**

> **A0 − A3 < 3 ms of p50 → this dies as a performance project.** It survives only on §1 (the pool is
> full) and ADR-0008. The frame-rate claim gets struck from the pitch.
> **3–8 ms** → proceeds, budgeted: measured march cost must come in under (A0 − A3).
> **> 8 ms** → proceeds unconditionally.
> **Spike > ~7.7 ms** → the marcher merely TIES the far cheaper heightfield project (see the ring
> ablation), and cannot justify itself on frame time. **This threshold is much tighter than the
> "> 11.5 ms means it costs more than PrePass+BasePass" one this plan originally carried**, because the
> right comparison is not "against doing nothing" but "against the cheapest alternative that also works".

### The spike — cheaper than expected, because it is already written

`VoxelFluidCollision.ush:265` is **`VoxelFluidWalkVoxelLine`** — a budgeted Amanatides & Woo walk over
the 512³ toroidal bit volume, mirroring `voxelcore/raycast.h`, **unit-tested** by
`tests/test_fluidoccupancy.cpp`, returning `{Hit, Voxel, FaceAxis, FaceSign, TEnter, BudgetExhausted}`.
`VoxelFluidRender.cpp:169` has the full-screen VS; `:470-495` has a working `RQT_AbsoluteTime` query ring
with an in-flight pool.

**The spike is a pixel shader and a cvar, not a system.** Arms: budget 1 (pass overhead alone), 64, 256,
886 (the full diagonal), 886 with the solidity test stubbed to `false` (**zero memory traffic**), plus
looking-down against at-the-horizon. **S4−S5 is the memory cost, S5−S1 is ALU, S7−S6 is divergence** —
the three numbers that decide the entire traversal design.

**What the spike cannot tell you, stated before its number is quoted:** single-level, dense, no palette
fetch, no ring transitions, no LOD, no sparse indirection. **It is a lower bound, not an estimate.**
Budget the real marcher at **1.4–1.6×** it.

---

## 3a. Phase 0 execution notes — what had to be fixed before a leg would run

Recorded because all four are pre-existing, none is obvious from a clean read, and together they cost
more time than the measurement itself.

1. **The editor could not boot at all.** `kWorldGenVersion` was 28 in `core.h:416` while
   `VXC_WORLDGEN_VERSION_USH` was 27, so the skew `#error` fired in every worldgen kernel and global
   shader compilation aborted. v28 (`51b18ce`) is asset-manifest only — it touches no `amplifier.cpp`,
   no `biome.h`, no `.ush` — so this is the "version lock, no math" dance, exactly as at v26 (`38bc780`,
   whose message records the identical boot failure). **Fixed:** mirror bumped to 28;
   `tools/compile-shaders.ps1` reports *"prebuilt SPIR-V matches this build"*, which is the compiled
   proof that no math moved. `kExpectedCpuDigestWorldGenVersion` was already 28.
   **This has now happened twice. It will happen again at v29 unless the bump is made part of the C++
   bump rather than a follow-up.**

2. **The determinism gate lied, convincingly.** `vxc_gpu --radius 64` reported
   `FAIL: 20 mismatch(es)`, `cpu=MAT_ROCK` vs `gpu=MAT_PERMAFROST`. That is not a divergence: the
   binaries under `voxel-core/build/bench/Release/` were timestamped **16 h before v27 landed**, so the
   exe ran pre-v27 CPU code against the current `.ush`, and the mismatch was precisely v27's removed
   dry-land cliff gate. Rebuilt, it reports `PASS: GPU output bit-exact`. Digests likewise: stale
   `53bd4391c21a89fb` against real `67383c809c179c60`.
   **Rebuild `vxc_*` before quoting any probe number — no engine build refreshes them.**

3. **The standard leg spawn has no tile coverage.** `-84480,53760` needs fine tile (-6,3); the current
   provider `…-b19d281fd` holds **15 tiles, all at negative Y**. `VoxelFineTileStreamer.cpp:891` is
   fatal on an unattended run by design (sea level is not a fallback), so the leg dies rather than
   degrading. **Consequence for this plan: Arm A cannot be compared against the 2026-07-28 baselines.**
   That is acceptable — A0 against A3 is a within-session A/B and is the comparison that matters — but
   no Phase 0 number should be quoted against a July number.

4. **The biome-vista poses are a different world.** `VS_temperate` (`-115200,-176640`) needs tile
   (-8,-12), which exists only under base `71e2b362…`, not the configured `80b9ca45…`. Different
   terrain-diffusion base, not merely a different bake. The configured provider *is* the newest
   (2026-08-17, bake_ver 28); coverage is simply thin.
   **Usable site:** `-VoxelSpawnAt` is in **METRES** (it is scaled by 100 into UU — `-61440` logs as
   `-6144000`). Fine pixels are **1.875 m** and a tile is 8192 px = **15.36 km**, which is why
   `DefaultGame.ini` says a tile-centre column sits 7,680 m from any edge. The configured namespace has a
   dense block at x∈{-3,-4,-5}, y∈{-4,-5}; the four-tile junction **(-61440, -61440) m** is covered on
   all quadrants, giving ~15 km of fine coverage in every direction — far more than R0 needs.

`tools/voxel-run-gpu-arm.ps1` gained `-SpawnAt` and `-Flight` parameters (defaults unchanged) so a leg
can be pointed at a covered site. **`static` is chosen here for A/B cleanliness, not for coverage** — at
15.36 km tiles a 120 s line flight at 20 m/s covers 2.4 km and stays inside one tile, so `line` is also
viable from this site. Static pins position *and* rotation and logs the pose, which is what an A/B of the
*draw path* wants: it removes streaming variance rather than averaging over it. (`EVoxelPerfFlight::Static`
exists because unpinned "settled" legs produced 43 fps and 103 fps on identical scenes.)

---

## 4. Memory — and a spread the census must close

Three independent estimates of the 10 cm / 4 km resident set disagreed by 4×: **~150 MB**, **~300 MB**,
**~657 MB**, depending on the assumed mixed-brick fraction (20–30%) and bits-per-voxel (2 against 4).
**That spread is why the census is a gate and not a paragraph.**

Per mixed 8³ brick: 64 B occupancy (`vxc::packBrickSolidBits`, `fluidoccupancy.h:161`) + ~16 B palette +
cells at 1/2/4/8 bpp. **Homogeneous bricks collapse to an 8 B descriptor with no payload — `tryCollapse`
is load-bearing, and without it the whole census fails.** Interior solid bricks, which greedy meshing
costs nothing for, collapse too, so the scheme does not lose the case it looks like it should lose.

Against **today's 2,197 MB (saturated, refusing 34,937 chunks)**, every estimate is a large refund.

**Measured 2026-08-19** (`vxc_volumeprobe`, full cascade, real tiles, no sampling) — every estimate below
is now replaced by a number, and all four landed inside the estimated bands:

| config | **measured (adaptive)** | earlier estimate | verdict |
|---|---|---|---|
| 10 cm, 4 km | **~415 MiB** | 150–660 MB | **fits easily; ~1.7× denser than quads on the same ground** |
| **5 cm in R0 only** | **1,110 MiB** | 280 MB–1.4 GB | **affordable — the target** |
| 2.5 cm, ≤64 m inner ring | **977 MiB** | 630 MB–2.2 GB | reachable |
| 5 cm across 4 km | 2,485 MiB | — | possible, not proposed |
| **2.5 cm across 4 km** | **16.5 GiB** | 13.8 GB+ | **strike confirmed** |

Composition at 10 cm: cells 128.5 · **flat index 193.3** · occupancy 56.0 · descriptors 23.1 · palette 14.0.
*(The census's 3.3 MiB palette line assumed a variable-size palette; the contract fixed it at 16 B, which
adds 10.7 MiB and takes the cascade to ~415 MiB — +2.6%, inside P2's ±20% gate.)*

**The "finer voxels" spend needs qualifying, and this is the one finding that genuinely constrains it.**
`Amplifier::column` is addressed in integer *level-0* voxels; there is no sub-100 mm horizontal query. So
a 5 cm sub-lattice is **exact in z and saturated in xy** — it doubles vertical fidelity and duplicates
horizontally. Real 5 cm horizontal detail is a **worldgen change**, not a renderer change. That does not
kill the spend (vertical detail on cliffs, overhangs and asset surfaces is most of what reads visually,
and the 5 cm detail-asset lattice lands natively), but "finer voxels" must not be sold as uniform 8×
detail. It is 2× in z, free in xy, and a worldgen project away from more.

Three structural notes:

- **The right implementation of "finer voxels" is to add ring levels *below* L0, not to rescale the
  cascade.** `gpu-g0-sizing.md` §3: *"The question is 'how far out do you want 10 cm?'"*

  **Measured 2026-08-19, and R0 is a smaller share than any estimate here assumed.** Quads resident per
  level at the control pose:

  | L0 | L1 | L2 | L3 | L4 | L5 |
  |---|---|---|---|---|---|
  | 8.60M | 7.08M | 8.77M | 8.24M | 7.30M | 6.81M |
  | 18.4% | 15.1% | 18.8% | 17.6% | 15.6% | 14.5% |

  The distribution is nearly uniform; no ring exceeds 18.8%. So **taking R0 from 10 cm to 5 cm multiplies
  R0's share by ~4 and the whole world by only ~1.55×**, not 4×. Finer voxels in the near field are
  markedly cheaper than the §4 table assumed. Conversely, **no single ring is where the cost is** —
  L3–L5 together are 47.7%.
- **At 2.5 cm the flat brick index alone exceeds an entire 10 cm world.** That forces a sparse hash or an
  octree — a design consequence, not a tuning one, and it must be decided now if 2.5 cm is ever wanted.
- **5 cm is a requirement, not a preference.** `VoxelDetailAssetSubsystem` resolves 85% of instances on
  the **5 cm detail lattice**, and `AssetGrid::onTerrainLattice()` returns false for them by design. The
  asset migration and "finer voxels" are the same work item. 2.5 cm buys these assets nothing.

**`vxc_volumeprobe`** (`voxel-core/bench/volumeprobe.cpp`, standard bench pattern) reports per ring level:
the brick census (air / solid / homogeneous / **mixed**); the **palette-size histogram over mixed bricks**
(the single number everything rests on); bytes under each bpp plus adaptive; **and the same walk's quad
count from `meshBrick<8>`**, so it is a comparison rather than two estimates — the `farwaterschemes.cpp`
precedent. It must refuse to report a cascade where any ring produced zero bricks, and print its own
sample size (a full 2.5 cm walk is ~365M bricks and is not runnable).

---

## 5. Render integration — verified against the 5.8 source

**The base pass cannot write depth, and this is provable.** With Nanite on, `ShouldForceFullDepthPass`
puts the base pass in `FExclusiveDepthStencil::DepthRead_StencilWrite` (`RendererScene.cpp:4408-4423`,
`DeferredShadingRenderer.cpp:2104-2113`). A screen-quad primitive writing `SV_Depth` in the base pass is
a dead end.

**Recommended: split across two hooks — the shape the engine itself ships on this platform.**

```
PreRenderView_RenderThread            stash view + prev matrices (per view)
PreRenderBasePass_RenderThread        [cs] tile classify -> indirect args
   (DeferredShadingRenderer.cpp:2791) [cs] march         -> VisBuffer + VoxelDepth
                                      reads prepass SceneDepth for t_max via FXRenderingUtils
   ... engine: DBuffer decals, shadow maps, Lumen scene lighting, base pass ...
PostRenderBasePassDeferred_RenderThread   [raster, indirect] emit SV_Depth + stencil,
   (BasePassRendering.cpp:1149-1156)      GBufferA..E, SceneColor, Velocity
```

This is structurally `Nanite::EmitDepthTargets`'s non-compute branch (`NaniteComposition.cpp:256`), which
**is** the PC path — `UseComputeDepthExport()` requires `GRHISupportsDepthUAV && GRHISupportsExplicitHTile`,
both false on D3D12 PC. The engine runs this exact shape every frame on this hardware.

Three things that must be done by hand and are easy to miss:

1. **DBuffer decals.** They are applied by the *base pass PS*; our emit runs after, so it must sample
   `DBufferA/B/C` and apply `DecodeDBufferData` / `ApplyDBufferData` itself (~15 lines from
   `DeferredDecal.ush`). Not optional — DBuffer is live (it takes over prepass forcing when Nanite is off).
2. **Velocity**, and the trap: use **`PrevPreViewTranslation`** with `PrevTranslatedWorldToClip`. Using
   the current `PreViewTranslation` produces a whole-screen velocity offset on exactly the frames the
   streaming origin rebases — the frames TSR smears worst. Get the target from
   `FXRenderingUtils::GetSceneVelocityTexture`; **do not clear it** (HISM and characters already wrote
   theirs). Reconstruct the hit from the ray's float `t`, never from the encoded depth.
3. **Stale HZB** — it is built before our depth lands, so **Lumen screen traces and SSR accelerate against
   an HZB with no terrain in it** and will overshoot. This is the one genuine quality debt of the seam and
   it touches owner decision #1. Falsify cheaply first: A/B `r.Lumen.ScreenProbeGather.ScreenTraces 0/1`
   on the *current* build — if it changes nothing today, it cannot be lost tomorrow.

Also budget **0.2–0.4 ms of HTILE decompression** for everything downstream that reads depth.

**Costs to keep, not regress:** the four TSR ini lines (`ThinGeometryDetection=1`,
`History.SampleCount=32`, translucency inside the resolve) fix a **screen-space** riser artefact that is
identical under a marcher. What improves: exact geometric normals, no vertex-rate anything, no LOD
skirts, and — uniquely — **the marcher can enforce "no voxel smaller than ~1.5 px" per ray**, which a
per-chunk mesher structurally cannot. What worsens: derivative-based mip selection (hand-solve with a
cone rule; ADR-0008 warns derivatives are discontinuous across voxel edges) and per-voxel jitter boiling
at range (fade it as `voxelSize/coneRadius → 1`).

**Ring skirts retire.** `ERingSkirtFace` and `voxel.GPU.VerifyRingSkirt` exist because two meshes at
different LODs leave a literal crack. A marcher has no mesh: the ray leaves ring L's grid and resumes in
ring L+1's at the same world `t`. The residual is not a hole but a **silhouette pop** (`mips.h` is solid
iff ≥4/8, so a thin feature can vanish one level up). Mitigation: overlap the rings by one chunk of
residency and take the first hit found in either level — ~2% more residency, exactly analogous to the
existing `UnloadRingMultiplier = 1.25`.

---

## 6. Shadows — one idea corrected, one baseline in doubt

**The baseline may not mean what it looks like.** The capture's 35,380,670 primitives is exactly
17,689,546 × 2, and 14,291 draws against 14,262 pool draws leaves **29 draws for the rest of the frame**.
`shadowGather=0` is separately recorded. So **the 18.4 ms baseline contains no terrain shadow casting** —
but the likely cause is **Virtual Shadow Map page caching** (Nanite on ⇒ VSM on by default; sun frozen at
`-VoxelTimeScale=0`; VSM's dynamic path does not route through the gather the census counts), not shadows
being off. **One-run falsifier, no code:** re-capture with `r.Shadow.Virtual.Enable 0`. If a ShadowDepths
pass with millions of primitives appears, the baseline is a *cached* frame.

Either way, quote the speedup twice: **~2× against the measured frame, ~3–5× against the same scene with
a live, moving sun** — the raster path degrades badly there and a marcher barely moves.

**Correction to the earlier draft — a receiver's material cannot march its own shadow ray.** In a deferred
renderer, direct sun is resolved in `RenderLights` from the GBuffer plus the light's screen shadow mask.
Nothing a DefaultLit material writes in the base pass survives into that multiply, and
`PrecomputedShadowFactors` is ignored for a movable directional light. **That idea is architecturally dead
and should not be attempted.**

**What works — and v1 gets it nearly free:** keep the greedy mesher alive **for L0–L1 only**, registered
with `SetVisibility(false)` + `SetCastHiddenShadow(true)`, so it draws **only** into shadow depth and
never into prepass or base pass. Camera primitives go to zero; every receiver — player proxy, agents,
debris, thrown items, water, ribbons — keeps correct terrain shadows with **no engine change**.
`voxel.Stream.GPUShadowMaxLevel` is already the knob. **And it keeps the old path alive and exercised,
which is the A/B control this project needs anyway.** A rare case where the conservative option is also
the cheap one.

Marched sun shadows (one wave-uniform secondary ray, +1–2.5 ms, no cascades, no acne, no peter-panning,
soft shadows free via cone jitter into TSR's 32-sample history) are **v2**, after the asset migration
shrinks the non-voxel receiver set to things within tens of metres of the camera.

---

## 7. Producer — `BrickPack`

**The chain maps 1:1 and the scan is reused verbatim.**

```
ColumnMain -> VoxelizeMain -> [AssetStamp*] -> BrickClassifyMain
   -> ScanBlocks/ScanSums/ScanAdd (UNCHANGED) -> BrickPackMain
   -> total readback -> AddBrickCompactPass -> AddBrickPoolWritePass
```

Determinism holds by the mesher's own argument: count → scan → emit, **no atomics in the output path**, so
order is identical by construction. (`InterlockedOr` into a material-present mask is fine — OR is
commutative, so the *result* is order-independent. The ban is on atomics whose value depends on order.)

**Three findings that change the work:**

1. **The halo dies — 3.375× less voxelize work per chunk.** `VoxelGpuChunkRegion` dispatches
   **48×48 columns × 6 bricks to produce 64**, because *"the mesher needs a one-brick halo to read
   apron/AO neighbours."* A marcher reads neighbours **by index**: normals come from the DDA face, AO from
   occupancy bits already in registers, colour from the voxel itself. The dispatch shrinks to
   **32×32×4**. Combined with deleting MeshCount/Scan/MeshEmit, **the producer gets cheaper, not more
   expensive.**
2. **Do NOT port `mips.h` to the GPU. The ring cascade already IS the LOD pyramid.** `VoxelizeMain`
   samples at `coarseRep(z, CoarseScale)`; R1–R5 *are* levels 1–5. Porting `downsampleBricks` would create
   a second, divergent definition of a coarse voxel, and `mips.h:11-16` warns the aggregation rule is
   worldgen-versioned and world-breaking to change. **Say no to this once, in writing, or it will be
   built.** *(Do use a separate MAX-aggregated 1-bit occupancy mip for GI cone marching — over-occlusion
   is the correct side to be wrong on for a digging game, and that is an occlusion rule, not a render-LOD
   rule.)*
3. **Edited chunks have no GPU `Cells` at all.** `ChunkHasEditedBrick` (`VoxelWorldSubsystem.cpp:7872`)
   routes them to the game-thread path, so a **second CPU brick-packing producer is required**. This is
   already solved once — `FVoxelFluidOccupancy` does exactly this via `vxc::packBrickSolidBits` over
   `vxc::World` including the overlay. **Reuse it, and put it in the plan on day one; it is a whole second
   path, not a bolt-on.**

**Where the code lives:** a new `voxel-core/shaders/brickpack.ush`, mirroring only the six-line
cell-indexing contract — the `VoxelAssetStamp.usf` precedent, whose header states the rule and the reason.
**The worldgen digest stays untouched and `vxc_gpu` keeps running the program it was pinned against.**

---

## 8. Streaming — what "apply a chunk" becomes

Today ~99% of apply cost is pool publication, growing 0.275 → **2.108 ms** within one leg (≈3 applies per
frame against a 6 ms budget). Tomorrow: allocate a brick range, one GPU→GPU copy, write **one**
chunk-table entry.

| term | today | after |
|---|---|---|
| `BuildChunkRuns` | 0.254 → 0.654 ms (game) | **gone** — it builds *draw ranges* |
| `RebuildRunBounds` | 0.247 → 0.714 ms (render) | **gone** |
| cull walk | ~1.0 ms/frame, 54,118 box tests | **gone** — visibility is a ray property |
| range emit | **~3.2 ms/frame** | **gone** — there are no ranges |

**~4.2 ms of the ~13.7 ms render thread — the one thread that can buy frame rate. And per the standing
rule that is a prediction until Arm A3 measures it.**

Consequences: **S1-2 handle recycling loses its justification** (it existed to bound `BuildChunkRuns`'s
append-only walk — 68,416 allocations to emit 18,389 runs). **T4-1 speculative generation keeps its whole
justification** — it exists because `queued p50 127.7 ms` against `dispatchToReady p50 12.1 ms`, a latency
gap the renderer does not touch — and should compound, since shorter queues cut the **40% mean stale
fraction**. Parking gets ~3× cheaper per chunk; measure before raising `PoolParkMax` (the observed peak
was 172 against a 4,000 cap, so it may not bind at all).

**The experiment that finds the new limiter, and it is cheap:** in P1, run the producer with `BrickPack`
on and publication *stubbed* — generate, pack, discard — and measure chunks/s.

---

## 9. Assets — and an honest accounting of the 35× regression

**Assets persist for free.** `VoxelAssetStamp.usf` already stamps resolved instances into `Cells` between
voxelize and mesh (RLE spans `z0:12|len:12|mat:8`, first-non-air-wins, with byte-identical CPU parity as
an identity). **At that instant the assets ARE voxels** — and today they are discarded with the transient.
`BrickPack` runs after `AssetStamp` in the same graph. **Zero new kernels, zero new stamping work.** The
coarse path (`AssetStampCoarseMain`, already at `coarseRep`) carries too.

Bonus: a greedy quad carries **one** material over up to 8×8 voxels, so per-voxel material is already lost
at the mesher. The volume is strictly *more* correct, not a compromise.

**The four causes of the 35× regression — 1 fixed, 1 defused, 2 untouched:**

| # | cause | verdict |
|---|---|---|
| 1 | ~9× vertical over-admission (13.48 layers against 1.45 needed) | **routed around, not fixed** — an over-admitted all-air chunk now packs to 64 descriptors (~512 B) and dispatches 3.375× cheaper. Costs ~9× the dispatches, ~0× the memory. |
| 2 | brick empty-test disabled | **fixed by construction** — the empty test *is* the homogeneous-collapse predicate, now running per brick in the packing kernel |
| 3 | global `std::mutex` in `AssetBankLibrary::bankGrid`, per air voxel per instance — **26,462,610 requests in one capture** | **NOT FIXED, NOT TOUCHED.** Entirely upstream, on the CPU resolution path. **It must be a separate named project, and this one must not absorb or hide it.** |
| 4 | crown-deleting correctness bug | **not fixed** — it becomes *visible* rather than masked. A benefit, not a fix. |

**Anyone pitching the marcher as "and it fixes asset streaming" is wrong by half.**

---

## 10. Verification — and the gate I got wrong

**Byte-equality against `vxc::Brick` is impossible as-is.** `Brick::paletteIndex` (`brick.h:114`) is
**insertion-ordered** — first-seen-first-index — so a GPU brick with a canonical ascending palette and a
CPU brick holding identical voxels hold **different bytes**. The earlier claim that Phase 1 "can be
verified exactly against a CPU-built reference" was wrong.

**The fix follows this repo's own precedent:** add `vxc::packChunkBricksCanonical()` to voxel-core —
engine-free, unit-tested, the single definition of the packed layout — and byte-compare the GPU against
*that*. This is exactly how `voxelcore/fluidoccupancy.h` relates to `VoxelFluidCollision.ush`. One header,
one test file. **Budget it in P1; do not discover it in P2.**

Gates, strongest first:

1. **`voxel.March.VerifyBricks`** — GPU readback against `packChunkBricksCanonical`. True byte identity,
   same shape as `voxel.GPU.VerifyPoolWrite` (direct + control + guard band).
2. **`voxel.March.VerifyQuads` — the strongest gate.** Regenerate the greedy quad stream *from the brick
   pool* and byte-compare it against the shipped mesher for the same chunk. Both derive from the same
   `Cells`, so **quad equality is volume equality**. No image, no runtime cost, and it proves the marcher
   is looking at the right world.
3. **Depth identity, not colour identity.** Pre-registered: the fraction of pixels with
   `|Δ linear depth| > 0.5 × voxelSize(level)` must be **< 0.5%**, *and* the failures must fall inside a
   dilated edge mask derived from the raster depth (both renderers are point samples of a step function at
   silhouettes).
4. **Colour statistics** via `tools/voxel-capture.ps1 -BurstCount` and `tools/imgdiff.py`, pre-registered
   against the recorded stability numbers: near terrain ≤ **0.92%**, far field ≤ **0.43%**.
5. **`voxel.Stream.CoverageVerify 1`** — unchanged; holes are holes however the surface is drawn.

Three-state cvar so the A/B is one binary in one session: `voxel.March 0` raster (control) / `1` marcher /
`2` both (raster to the real targets, marcher to scratch, gates run).

Per `[[voxelsim-owner-judges-screenshots]]`: A/B pairs and conditions go to the owner. I do not declare
the look acceptable.

---

## 11. Phasing

| # | phase | must measure to earn the next |
|---|---|---|
| **P0** | gate: spike, census, arms A–C, two debug cvars — **substantially complete; see the status board** | A0−A3; the serial slope; the 9.5 ms attribution; ns/pixel with its ALU / memory / divergence split; mixed-brick fraction and palette histogram |
| **P0.5** | **the step-count experiment** — a coarse occupancy mip over the existing 512³ volume and a hierarchical march, so `steps(flat) / steps(skipped)` is *measured*. One reduce pass and a skip loop over a volume that already exists; no brick pool, no producer, no render integration | the skip ratio. Feed it into `0.13 + steps × 5.9 µs` and the 4 km march stops being a guess |
| **P1-C / P2** | brick pool residency + dispatch + `VerifyBrickPack` | **BUILT.** `FVoxelBrickPool` (3 arenas + chunk table, `FVoxelGpuGeometryPool` unforked), dispatch behind `voxel.GPU.BrickPack`, byte-equality gate with control + direct + guard band. **VRAM gate corrected to 223 MiB, not 415** — see below. Awaiting in-engine verify |
| **P3** | the marcher DRAWS — depth + GBuffer via the two-hook seam, behind `voxel.March 0/1/2` | **BUILT.** Two-hook seam holds; `voxel.March.VerifyDepth` implements §10 gate 3. Source is still the fluid occupancy volume (~25.6 m, no per-voxel material), so **the depth gate is meaningful and colour gates are not** until P2's source swap |
| **P1** | **P1-A and P1-B COMPLETE.** `vxc::packChunkBricksCanonical` (15 tests, 726 pass, mutation-checked) and `brickpack.ush` (8 permutations, DXIL + SPIR-V) **byte-compared over 16 chunks / 1,024 bricks with 0 mismatches**, across bpp 0/1/2/4/8 and both uniform kinds. The contract is **PROVEN** and gained 7 corrections from the process. **P1-C (UE dispatch + `VerifyBricks`) is unblocked** | byte-identical on 100% of a settled cascade across all six biome poses; added GPU cost; **live histogram against the census prediction**; `vxc_gpu` re-run with the new chain; **chunks/s with publication stubbed** |
| **P2** | brick pool residency, not drawn; **chunk table addressable by `(level, brickCoord)`** (a one-line requirement now, a rewrite in P7) | real VRAM at settle against the census **within ±20%** — outside that, **stop and re-derive**; fragmentation; `allocFail=0` |
| **P3** | marcher draws behind `voxel.March`, quads still resident | p50/p95 marcher against quads on the same leg pair; pixel diff at the project bar; does the march land where PrePass+BasePass were, and cost less |
| **P4** | drop the terrain quad path; mesher demoted to **shadow-only caster** (L0–L1, `SetCastHiddenShadow`) | the ~4.2 ms must be **gone, not moved**; **pool refusals 0 at the temperate pose**; VRAM |
| **P5** | asset verification and admission retune | assets-on settle against the 130 s terrain-only baseline. **35× must become ≤2×** — if not, the `bankGrid` mutex is confirmed as the real limiter and becomes its own project |
| **P6** | finer voxels in R0, one ring at a time (`-VoxelRingVoxelSizeMm=`, command line not cvar — ring geometry must be known before the first `RecomputeDesiredSet`) | VRAM against the census; frame at 5 cm; producer throughput at 5 cm |
| **P7** | ~~GI cone march over the same bricks~~ **RETIRED AS A PERFORMANCE PROJECT 2026-08-20 — see §13.** Re-scoped to streaming cost + two correctness bugs | ~~p95 and hitch count~~ **struck.** Now: `quadsRetainedForGI` at fill, and the volume-anchoring bugs |

---

## 12. What is not affordable — the blunt list

1. **2.5 cm across the 4 km cascade.** 13.8 GB+. Not fixable by better packing — the index and skip masks
   alone exceed an entire 10 cm world. **Strike it now, not at month four.**
2. **The current quad pool, at the world already being generated.** 200M saturated, 34,937 chunks refused,
   268M behind an int32 audit. Already unaffordable, today, in the shipping default.
3. **Byte-equality against `vxc::Brick` verbatim.** Insertion-ordered palettes. It needs a canonical packer.
4. **Any frame-rate claim before Arm A3 runs.** Seven predictions, seven falsifications.
5. **Fixing asset streaming as a side effect.** Two of the four causes are untouched, and one is a genuine
   blocker for "terrain + assets in one volume".
6. **92,000 chunks/s as a target.** Mesher independence is already proven (265.7 against 261.1 chunks/s).
   The prize from removing apply cost is real and large; it is not 86×.

---

## 13. P7 — GI: retired as a performance project, re-scoped. MEASURED 2026-08-20.

### The result, against a rule fixed before the data existed

| arm | config | p50 |
|---|---|---|
| **G0** | shipped defaults, GI on | **34.79 ms** |
| **G1** | `-VoxelGIOff` (arm verified HELD) | **34.71 ms** |
| **G2** | GI on, `MaxBrickSolvesPerFrame 0` + `RefreshBricksPerFrame 0` | **34.90 ms** |

**G0 − G1 = 0.08 ms.** Three-arm spread **0.19 ms**. G2 — GI on with the cone march *disabled* — is the
**slowest** of the three, which is the clearest possible statement that the ordering is noise. G2's
`GI phase:` line reads `solve 0.00ms/0 bricks`, so the arm is proven to have done what it claims rather
than assumed to.

The rule, stated in advance and quoted verbatim:

> **G0 − G1 < 2× the G0 spread → GI is already effectively free on the pooled path, Phase 7 is a
> correctness and coverage project, not a performance one, and the p95 framing gets struck.**

That is the branch. **Struck, not footnoted.**

### What the retired number actually described

**+14.9% p95 / ×3.2 hitches described a renderer nobody ships.** It was measured on the **component**
path (`voxel.Stream.GPU 0`) with `voxel.GI.Volume 0`. Its dominant attributed term is the per-chunk
vertex-colour re-shade (`docs/status.md:4994-5008`: `MaxChunkRefreshesPerFrame 0` removes 1.7 of 4.2 ms),
and that re-shade **structurally cannot occur on the pooled path** — `VoxelGI.cpp` only enqueues a brick
for re-shade if `BrickComponents.Contains(Key)`, and pooled chunks never get an entry. The second term,
per-vertex CPU `SampleIrradiance` during proxy builds, is likewise absent: the pooled factory samples a
volume texture instead. Two of the three measured terms were already gone before this measurement ran.

**A note on the hitch counter, because it silently changed meaning.** `hitchThresholdMs` is **33.3** and
post-warmup **p50 is 34.71** on the shadowed baseline — the *median* frame now exceeds the hitch
threshold, so ~65% of frames count as hitches for the mundane reason that the scene runs at 28.8 fps. In
the records this number came from, p50 was 23.3 and a hitch genuinely meant a spike. **Hitch counts are
not comparable across the shadow discontinuity** and should not be quoted against the old ×3.2 without
re-basing the threshold — which would in turn re-base every historical hitch figure. Judge on p95 and
`postWarmupMaxFrameMs`. (`postWarmupMaxFrameMs` was 275.3 on G1, so the 400 ms clamp did **not** fire
post-warmup and G1's p50/p95 are valid; the `max=400.00` in the one-line summary is the warmup window.)

### What survives, and it is not frame time

**1. Quad retention — a cost to STREAMING, which is where it hid.** With GI on, every level-0 chunk
within `RadiusUU × 1.25` = **87.5 m** is forced off the D1 direct-to-pool path onto the GPU readback
path, purely so the light field can have its quads (`VoxelWorldSubsystem.cpp:10389` →
`VoxelGI.cpp WantsChunkQuads`). Measured at fill by the new counter: **806 of 1,319 level-0 candidates,
61%**. At settle it is **zero** — which is exactly why every previous GI measurement missed it, all of
them being settled static poses. It lands on a GPU mesh fork already measured as throughput-bound.

Feeding the light field from the **resident brick volume** deletes this outright. The volume is
byte-proven, **99.4%** covered including the edit path, and free (35.4 s control vs 35.6 s cold fill).
**That is now the entire performance case for Phase 7, and it is a loading case, not a frame case.**

**2. Two volume-anchoring correctness bugs, both silent, both found while chasing a crash.**

- **The volume-origin latch.** `EnsureVolumeOrigin` latches (`bVolumeOriginSet`) and derives
  `VolumeOriginWorldUU` from `FieldCentreUU`. `ResolveViewOriginUU` returned
  `Cam->GetCameraLocation()` without checking that the camera cache had ever been updated — and that
  cache is exactly `FVector::ZeroVector` until it has. On frame 0 under `voxel.Stream.GPU 1`, where a
  pool can already exist, that anchors the GI volume **6,144 km from the camera, permanently**. The
  engine states the required guard in its own canonical accessor,
  `APlayerController::GetPlayerViewPoint`: `PlayerCameraManager->GetCameraCacheTime() > 0.f
  // Whether camera was updated at least once`. **FIXED 2026-08-19** (guard added, falls through to the
  pawn; `EnsureVolumeOrigin` additionally refuses to latch from an unresolved centre).
  The same zero centre also made the `voxel.GI.Debug 1` diagnostic query elevation at the world origin,
  which killed leg `g0-gion-r1` on frame 0 via the fine-tier gate; that probe now sits behind
  `voxel.GI.Debug >= 2` because a once-per-second diagnostic must never be able to end an unattended run.

- **`FindPoolWorldLocation` picks the first pool `TObjectIterator` returns.** **STILL OPEN.** Four sites
  construct a `UVoxelGpuPoolComponent` — terrain, the **water pool (one per bucket)**, and two verify
  harnesses — and they sit at **different world locations** by construction: terrain
  `SetWorldLocation(GpuPoolRebase = FirstChunkOrigin)` (`VoxelWorldSubsystem.cpp:12275`), water
  `SetWorldLocation(WaterPoolBucketRebase(BucketKey))` (`VoxelWaterSubsystem.cpp:2075`). Picking a water
  bucket offsets `OriginPoolUU` by the difference of two unrelated rebases, and the symptom is missing or
  misplaced lighting, never an error. A candidate census + `Warning` now makes the pick auditable from a
  log; **behaviour is deliberately unchanged**, because water pools always exist and refusing outright
  would trade a silent misplacement for a silent feature loss.

  **The deeper form of the same problem, recorded so it is not conflated:** `FVoxelGIVolumeParameters` is
  a **global** uniform buffer with **one** `OriginPoolUU`, and `VoxelQuadVertexFactory.ush:525` computes
  `GIUVW = (ProbePos - VoxelGIVol.OriginPoolUU) * InvSizeUU` with `ProbePos` in *whichever pool is
  drawing*. **One pool-space origin cannot be correct for two pools at different rebases, however the
  iterator picks.** That is a design question for the volume, not a bug in the finder.

### Re-scoped P7

| # | item | shape | earns its place by |
|---|---|---|---|
| **P7-a** | fix `FindPoolWorldLocation` to select the **terrain** pool by identity | needs a public pool-identity accessor (`GetPoolName()` or equivalent) on `UVoxelGpuPoolComponent` — one line, but in another workstream's file | the Warning stops firing; a pinned-pose capture is unchanged |
| **P7-b** | decide what one global `OriginPoolUU` means for multiple pools | design, not code | stated answer in this doc |
| **P7-c** | feed the light field from the brick volume, **for streaming** | `voxel.GI.SourceBricks 0/1`, one binary, A/B | `quadsRetainedForGI` → 0 at fill with packs non-zero; **cold fill** against the 35.4 s control |
| ~~P7-d~~ | ~~move the cone march to the GPU~~ | **not justified.** G2 measured the march at noise on the pooled path | — |

**The correctness gate for P7-c is unchanged and still applies** (one-directional containment: every cell
the quad path marks opaque must be marked opaque by the brick path, `violations / cellsSetByQuads`,
expected exactly 0/N; the reverse direction reported as a measurement; `chunksMissingFromPool` as a
refuse-to-pass condition). **A missing chunk reads as AIR, not as an error, and a partially resident
volume produces a fully lit field that looks exactly like GI being off.**
