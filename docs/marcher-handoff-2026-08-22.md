# Handoff: voxel streaming pipeline — the plan, the evidence, and the day's fixes

**Date:** 2026-08-22 (end of day, rewritten for handover)
**Branch:** `marcher/wave1-2-shading-and-tick-ceiling`, merged to `main`
**Working dir:** `D:\voxelsim` (the single checkout — `vox-int` is dead, do not work there)

This is the whole context in one file. **Start at §1, then execute §5.**

---

## 1. What the owner wants, and where we are

The marcher is the primary terrain renderer and its **draw path is not the problem** —
`renderMs` is 7–11 ms. The owner flies at 30 m/s and sees **ring-shaped gaps at every LOD
boundary and large holes that take seconds to fill**. The pipeline feeding the renderer is the
problem.

**The goal is not "adequate".** The owner's words: *an incredible, performant, fast, high-FPS
pipeline… so fast that the player never sees holes.* 6,200 chunks/s is the **floor**.

### The requirement, derived

`resident0 = 41,625` level-0 chunks over a 128 m disc ⇒ **8.3 chunks per vertical column**
(51,472 m² ÷ 10.24 m² per 3.2 m footprint = 5,026 columns). New ground enters ring 0 at
`2 × R × v` m²/s:

| Speed | Columns/s | **Level-0 chunks/s** |
|---|---|---|
| 6 m/s (walk) | 150 | 1,245 |
| **30 m/s (current fly)** | 750 | **6,200 — the FLOOR** |
| 100 m/s | 2,500 | 20,750 |
| 240 m/s | 6,000 | 49,800 |

Measured today: **968–2,435 chunks/s** — a 3–6× shortfall at the floor. This predicts the
owner's exact experience: fine standing still, marginal walking, badly behind flying.

**Stretch target 20,000–50,000 chunks/s**, and it is not arbitrary: at ~3 KB of pool data per
chunk, 50,000 chunks/s is **150 MB/s** of VRAM writes. The GPU already holds a bit-exact mirror
of the column generator. The current cost is structural, not fundamental.

### Owner decisions (do not relitigate)

- **Coarser terrain, never holes.** A late chunk degrades to lower detail, never a gap.
- **30 m/s must be flawless**; go as far beyond as the design allows.
- **Memory may grow to 1–2 GB VRAM** (pool is 131,072 chunks / ~388 MiB today).

---

## 2. Where the time actually goes (measured, not assumed)

| Finding | Evidence |
|---|---|
| **The 34×34 column grid is ~72% of a level-0 job's retired cycles** | `gridKcyc=3592` of `jobKcyc=4980`, `cycPerColumn≈3107` (`Saved/final-shipped-state.log`) |
| **It is rebuilt per chunk; the 8–16 Z-siblings rebuild it identically** | `VoxelWorldSubsystem.cpp:12196-12203` — `Key.Z` never enters it. The file concedes it at `:1655-1657` |
| **Worker threads are descheduled ~99% of their life** | `jobGHz=0.04`. `MaxJobsInFlight = 8 × cores = 96` (`:11515`) vs **~12 real background threads** (`:2756`) |
| **`Amplifier::column` cost roughly doubled** since 2026-07-28 | `cycPerColumn` 1,517 → 3,107 |
| The GPU meshing path carries **under 5%** of the work | `brickFromGpu=29,798` of `brickPacks=651,584` |
| `FSharedMipCache` (512 MB budget) is **dead code** at defaults | Only reachable via `MakeLevelSampler`; `kDefaultCoarseMinLevel=1` routes past it |
| Game-thread streaming tick was **40–95% of wall** | `Voxel tick budget` lines |

**Do NOT raise `voxel.GPU.MeshBatchCap`.** Its own comment records a measured sweep: 32/64 gave
*367 hitches / 77.2k chunks* against 4/8's *8 hitches / 89.4k*. Bigger per-chunk batches were
tried and are worse — because **each job adds ~7 compute passes + 3–4 copies**. That is the
real ceiling, and Tier B removes it.

---

## 3. Fixed today (do not re-diagnose)

| Fix | What it was |
|---|---|
| **PIE teardown** | Pool, march chunk index **and the marcher's view extension** all outlived the `UWorld`. Three instances of one root cause. The extension one produced "lakes floating in an empty sky": terrain streamed and nothing drew it, while water kept rendering through its own quad pools. Gate: `Voxel GPU teardown: … (pool now 0, index now 0)` and the march extension must register in **every** PIE session |
| **`airProof` 250 ms → 0.03 ms** | `FootprintSurfaceUpperBoundMm` was the only worldgen-grade call on the dispatch path not memoised. Its call-site comment predicted it. Residency-gated, because a stale bound under-states the surface and that is *holes*, not staleness |
| **Ring gaps, cause 1** | The marcher segmented rings by **3D distance from camera**; streaming admits by **horizontal distance**. Spheres vs cylinders, diverging by camera height. Found by flying straight up. Fixed with `t = R / \|Dir.xy\|` |
| **Ring gaps, cause 2** | The **inner** ring edge was unpadded while the outer was padded, so coarse chunks whose centre fell inside `Inner` were skipped although they extend past it. Holes were exactly one chunk square |
| **GPU job timeout clock** | Timed from *submit* (queue entry), not promotion, against a budget whose message says "readback not ready". **8,984 dispatched, 4,480 timed out (~50%)**, self-reinforcing. Now timed from promotion |
| **Editor throttled PIE to 3 FPS** | `bThrottleCPUWhenNotForeground`. `frameMs=333.33` repeated exactly. A voxel world streams on the game thread, so a frame cap **is** a streaming cap. Disabled in both `Config/` and `Saved/Config/` |
| **FPS readout** | `voxel.Debug 1` panel, top line: rate, mean ms, **1% low**, worst frame. Wall-clock timed, 4 Hz |
| **`voxel.March.ClimateStrength` → 0** | At the owner's request, so the blue speckling can be judged with the tint out. Set to 1 to A/B it |

## 4. Two things I got wrong today — do not repeat

1. **Raising `voxel.Stream.RingOverlapChunks` 0 → 1.** Intended to close the ring gaps.
   The owner tested it and **the gaps were still there**, and `VoxelBrickTraverse.ush` already
   recorded its cost: *+9.2% resident chunks, chunks/s 968 → 672, hitches 1 → 47*. Reverted.
   It stays available as a bisection tool only.
2. **The inner-edge pad introduced a thrash band.** Admission now pads inward
   (`:9636-9640`) but the exit scan still evicts at raw `InnerUU` (`:9313`), so that band is
   admitted and evicted forever — the HUD's **11,779 unloads/s with the player stationary**.
   **This is Phase 0.1 below and is the first thing to fix.**

Also: I attributed a bottleneck to `gpuMgrTick` from the **wrong 5-second log window** and was
wrong. Check timestamps. A number repeating *exactly* across windows (`333.33`, `21000.65`,
`400.00`) is a clamp, cap or stall — never a measurement.

---

## 5. THE PLAN

### TIER A — Clear the floor (6,200 chunks/s)

#### Phase 0 — Stop the bleeding (cheap, do first)

**0.1 Fix the inner-edge admit/evict thrash.** Give the exit test (`:9313`) the same pad
admission uses plus hysteresis, and extend the "admit band must not reach the evict band"
check that already guards the outer edge (`:9565-9578`) to cover the inner one.
**Gate: stationary `unloaded/s` falls from ~11,779 to ~0.**

**0.2 Fix the two lying instruments.** The HUD's cap readout uses a stale `2 × cores`
(`:15960`) while the real cap is `8 × cores` — it displayed `24` against a real cap of 96. And
`Result.JobMs` is *service time* on the CPU path (`:12724`) but *end-to-end latency including
queue wait* on the GPU path (`:11386`), blended into one window. Split them.

#### Phase 1 — The no-hole invariant (the architectural fix)

Today **exactly one level covers any given ground** — `if (Level > 0 && DistSq < Inner) continue;`
skips coarse chunks inside the finer ring. **So a late fine chunk IS a hole, by construction.**
No amount of throughput removes that; it only makes it rarer.

Make every coarser level also cover the ground inside it; the marcher walks fine → coarse and
takes the first hit. A missing level-0 chunk then renders as level-1 detail.

**This is cheap.** Each coarser level is half-resolution in three dimensions ⇒ 1/8 the chunks
per volume. Full hierarchical coverage costs `1/8 + 1/64 + … ≈ 14%` more resident chunks.

- Admission: drop the inner-skip for levels ≥ 1; rings become *scheduling priority*, not
  coverage. Sites `:9610` and the verifier `:10776`.
- Marcher: the ring walk (`VoxelBrickTraverse.ush:2662`) already iterates levels in order — on
  a miss at level L, fall through to L+1 for that segment instead of advancing past it.
- Eviction must never drop a coarse chunk whose finer replacement is not yet resident.

#### Phase 2 — Share the column grid across Z-siblings

The biggest single CPU win: 1,156 `Amplifier::column` calls per chunk, ~72% of job cycles, and
8–16 stacked chunks each rebuild the *same* grid.

- **Run `-VoxelL0GridCacheProbe` FIRST.** It ships (`:11875-11901`), reports
  `distinct/dispatches`, and **has never been run**. One leg measures the exact reuse ratio and
  decides whether to build the cache at all.
- Add a shared refcounted `(Level, ChunkX, ChunkY) → column grid` cache at `:12196`, replacing
  the `static thread_local` scratch at `:12179` (an allocation optimisation, not a memo).
  208 KB/entry ⇒ 1,024 entries ≈ 208 MB.
- **Residency-gate it** exactly as `FootprintChunkZRangeCached` is (`:8792-8819`) — a grid
  computed while a fine tile is decoding comes from the sea-level fallback, and a stale
  *column grid* means wrong terrain.
- Same for `FCoarseChunkGridSampler` (`:12636`), which rebuilds 1,156 coarse columns per chunk.

Expected: level-0 job ~1.5 ms → ~0.5 ms, a **2.5–3× throughput multiplier**.

#### Phase 3 — Stop oversubscribing the worker pool

96 in-flight jobs against ~12 real background threads (`jobGHz=0.04`). Also why "worker ms"
reads in seconds and why GPU jobs time out.

- Try `voxel.Stream.JobsInFlightPerCore` 8 → 2 and measure. The 2 → 8 raise predates the
  brick/marcher path entirely.
- Fix the accounting asymmetries: GPU jobs consume ring-floor slots but are subtracted from the
  CPU budget (`:11557-11563`), and **speculative jobs enter `GpuJobsPending` without touching
  `JobsInFlightCounter`**, so the loop over-dispatches by up to `SpeculativeMaxInFlight` while
  `FMath::Max(0, …)` hides the negative.
- `ParkSpeculativeResult` runs **inline on the game thread** inside the manager tick, measured
  at **334.7 ms of a 343.9 ms dispatch bucket** (`:5884-5898`). Move it off or budget it.

### TIER B — GPU-resident streaming (20,000–50,000 chunks/s)

Tier A removes waste from a CPU pipeline. Tier B removes the CPU from the steady state.
**Do not start until Tier A is measured.**

**B.1 — Amortise GPU passes across chunks, not one graph per chunk.** The core change, and the
direct answer to why raising `MeshBatchCap` hitched. Today N chunks cost N × (7 compute + 3–4
copy) passes. Restructure so **one dispatch generates many chunks**: a persistent worklist
buffer of pending requests, one indirect dispatch per stage over all of them, passes constant
in N. The kernels already exist (`VoxelGpuWorldGen.cpp:1089 AddRegionPasses`); what changes is
region granularity and dispatch shape, not the maths.

**B.2 — Keep the whole loop on the GPU with no readback.** `worldgen.ush:1839 ColumnMain` is
already a **bit-exact mirror** of `Amplifier::column`, and direct-to-pool brick packing exists.
Steady state should be: CPU uploads camera + a compact request list; GPU generates columns,
voxelizes, packs bricks into the pool; nothing comes back.

**B.3 — Get the asset resolve off the game thread.** For a GPU job it still runs there
(`:11099-11200`), including an `Amp.column` per candidate site. Move it to the worker pool or
fold it into the GPU stamp passes (`AssetStampMain` exists).

**B.4 — GPU-side desired-set maintenance (stretch).** Removes the last per-chunk CPU term.

**Ceiling check:** 50,000 chunks/s × 1,156 columns = 58M columns/s — a modest GPU load. The
honest unknown is dispatch overhead and pool allocator contention, not ALU.

---

## 6. Verification

Two legs per arm, per the standing rule. **Read the log after every session and lead with it.**

1. **Phase 0 gate** — stationary PIE 60 s: `unloaded/s` ~11,779 → ~0. A gate that can fail.
2. **Phase 2 pre-work** — `-VoxelL0GridCacheProbe` leg; report `distinct/dispatches`.
3. **Throughput** — fly at 30 m/s, read `brickPacks/s` from `Voxel tick budget`. Tier A gate:
   ≥ 6,200/s sustained. Tier B: report the maximum reached and the speed it supports.
4. **The no-hole gate** — needs a real metric, not an eyeball. Add a counter for *"ray exited
   the cascade with no hit inside the outer ring"*; fly at 30 m/s; target zero.
5. **Owner judgement** — captures at ring boundaries and a Stop→Play cycle. The owner judges
   the look; present captures and conditions, never a verdict.

## 7. How to run the editor (this cost an hour — read it)

**This project has NO level asset. Zero `.umap` files.** The world is built in code by
`AVoxelEarthGameMode::BeginPlay()`. The editor opens an empty "Untitled" world with default UE
landscape — **that is correct**; `BeginPlay` has not run. Terrain exists only in **PIE or
`-game`**. The spawn arg is **not optional** — it defaults to (0,0), outside baked tiles.

```
D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe "D:\voxelsim\ue-project\VoxelEarth.uproject" -dx12 -sm6 -VoxelSpawnAt=-61440,-61440 -VoxelNoClipmap
```

## 8. Rules that will bite you

- **One Unreal editor per box.** Check with `Get-Process`, never `tasklist` via Bash.
- **A live editor blocks builds** (LNK1104). Builds and legs cannot overlap either way.
- **`.usf` files are live input to the next run.** Stamp with `tools/voxel-stamp-build.ps1`
  only after a SUCCESSFUL build — `&& stamp`, never `; stamp`.
- **`VoxelEarthShaders` may not depend on `VoxelEarth`.** Mirror cvars instead.
- **Adaptive unity hides anon-namespace collisions.** `tools/lint-unity-collisions.py` misses some.
- **Never read a pre-ROP counter as evidence pixels landed.**
- **The owner judges screenshots.** Present captures and conditions, never a verdict.
- **Plain English.** Lead with the effect.

## 9. Still open, not in the plan

| Item | State |
|---|---|
| Blue speckling on terrain | Owner-reported with screenshots; worse at coarse LOD, worst under the camera. `ClimateStrength` now 0 — if blue persists, it is the material lookup or the GI probe |
| GI shows no visible difference | Origin verified CORRECT (`delta (0,0,0)`). Volume covers only **±38 m, fades out at 35 m** — A/B must be on the ground within ~20 m. Discriminate with `voxel.GI.DebugVis 1` |
| Marched shadows | Work, but **7–9 ms at 512 m reach**. Dial is `voxel.Shadow.MarchRayReachM` (48/96/256). Reach choice never made |
| Wave 5 — water off the quad pool | Not started. Turns retirement into deletion |
| Depth gate | 27.98% interior disagreement, unexplained |
| ~28 never-audited files | Committed to preserve them, not because they were reviewed |

## 10. Sources consulted

- [Aokana: A GPU-Driven Voxel Rendering Framework for Open World Games](https://arxiv.org/abs/2505.02017)
- [Vulkan Guide — high-performance voxel and mesh rendering](https://vkguide.dev/docs/ascendant/ascendant_geometry/)
- [High Performance Voxel Engine: Vertex Pooling](https://nickmcd.me/2021/04/04/high-performance-voxel-engine/)
- [Voxel Tools (Zylann) — VoxelLodTerrain](https://voxel-tools.readthedocs.io/en/latest/api/VoxelLodTerrain/)
- [A level of detail method for blocky voxels — 0 FPS](https://0fps.net/2018/03/03/a-level-of-detail-method-for-blocky-voxels/)
