# Handoff: GPU ray marcher as primary terrain renderer

**Date:** 2026-08-22 (rewritten end of day)
**Branch:** `marcher/wave1-2-shading-and-tick-ceiling` (NOT pushed)
**Working dir:** `D:\voxelsim` (the single checkout — `vox-int` is dead, do not work there)

---

## 0. READ THIS FIRST — where the bottleneck actually is

The owner asked the right question: *"the marcher was supposed to give 2x cascade distance and
2x FPS, but the game feels worse — is that the rendering side or the streaming side?"*

**It is the streaming side, on the game thread. The marcher is not the problem, and making it
faster will not help.** Measured in the editor 2026-08-22:

| | |
|---|---|
| `renderMs` | **7.4 – 10.9 ms** |
| `rhiMs` | 2.9 – 6.0 ms |
| `subsystemTickMs` (voxel, game thread) | **10 – 64 ms** |
| voxel streaming tick, share of wall during fill | **40 – 79%** |

Rendering is ~8-11 ms and is not the constraint. The game-thread streaming tick is.

**Why the 2.72x is both real and misleading — this is the important part.** That number was
measured on a **settled world with a static camera**, 90 s preflight before a 60 s run, no
fluid, shadows off in both arms. Under those conditions streaming is idle and the draw path is
the entire frame, so the draw path is what the ratio measures. It is an honest number for the
question it was asked.

**Live play does not meet those conditions.** The camera moves, streaming is saturated, and the
game thread dominates. The marcher's win is invisible there — not because it failed, but
because it optimised something that was not the live bottleneck. **Every marcher-vs-quad
measurement in this programme shares that flaw.** Before quoting any of them again, ask what
the measurement's settling period excluded.

So: the marcher delivered a faster draw path, and the draw path was not what the owner feels.

---

## 1. How to actually see the thing (do not skip)

**This project has NO level asset. Zero `.umap` files.** The world is built in code by
`AVoxelEarthGameMode::BeginPlay()`; `GlobalDefaultGameMode` points at it and
`GameDefaultMap=/Engine/Maps/Entry.Entry`.

- The editor opens an empty "Untitled" world with default UE landscape. **That is correct** —
  `BeginPlay` has not run. Terrain exists only in **PIE or `-game`**.
- **The spawn arg is not optional.** It defaults to (0,0), outside this seed's baked tiles.

```
D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe "D:\voxelsim\ue-project\VoxelEarth.uproject" -dx12 -sm6 -VoxelSpawnAt=-61440,-61440 -VoxelNoClipmap
```

Press Play, allow 30-60 s to settle.

---

## 2. Fixed today, with evidence

### 2.1 PIE teardown — the pool and index outlived the world

A second PIE session rendered pure sky. Three links, and **both teardown primitives already
existed with ZERO callers**:

1. `UVoxelWorldSubsystem::Deinitialize` never released anything.
2. `FVoxelBrickPool::Reset` and `FVoxelMarchChunkIndex::Detach` were written for teardown and
   never wired up.
3. `AttachToGlobalPool`'s `Cells.SetNumZeroed(kCells)` was a **no-op on re-attach** —
   `TArray::SetNumZeroed` only zeroes elements it ADDS — so the 4 MiB grid kept every stale
   resident bit. `Seed()` resets the counters but never the cells.

Gated by a line that can fail (it logs the AFTER counts):
`Voxel GPU teardown: released 96310 pool chunks and 96310 index entries (pool now 0, index now 0)`.

Why no leg caught it: **every leg is a fresh `-game` process**, so the pool was always empty at
start. PIE re-entry is the only path that starts a world against a populated pool.

`FVoxelMarchChunkIndex::Detach` also releases the pooled GPU buffer through
`ENQUEUE_RENDER_COMMAND` — `Register()` writes it via `QueueBufferExtraction` on the render
thread, so clearing it game-side was a real race, and leaving it alone would hand the next
world the previous world's index buffer.

**Open consequence:** teardown now DISCARDS ~96,000 chunks that were still valid geometry for
the same world at the same location, so a second Play is a full cold re-stream. Re-adopting
them would make it near-instant — but that would MASK the throughput problems below, so it
must wait.

### 2.2 The sky-band bound was the dispatch bottleneck — `airProof` 250 ms → 0.03 ms

`IsChunkProvablyAllAir` → `FootprintSurfaceUpperBoundMm` was **the only worldgen-grade call on
the dispatch path that was not memoized**, and the call site's own comment said so, ending
"whether that is where the 0.21 ms/chunk lives is a HYPOTHESIS until this number is read".

Read: `airProofMs=245-261` of a `dispatchMs=275-288`, with the streaming tick at **95% of wall
and 170 ms per tick**. Every chunk in a column recomputed a value that depends only on the
column. Its two siblings twenty lines away were both memoized; this one was missed.

Fixed with `FootprintSurfaceUpperBoundCache`, keyed per footprint, both call sites routed
through it (which helps the cold-fill `recompute` path too). **Measured after:
`airProofMs=0.02-0.03`.**

**The trap, and why it is not cached unconditionally.** The bound reads the same rasters as the
z-range memo, so an entry computed while a fine tile is still decoding comes from the sea-level
fallback. A stale z-range gives a wrong band; a stale bound here **under-states the surface**,
and the wrapped function's comment says what that costs: *"a chunk containing terrain is never
generated. A hole in the world."* It is residency-gated like `FootprintChunkZRangeCached`, and
widened by asset reach at **every** level rather than just level 0, because
`assetAwareSurfaceUpperBoundMm` gates on the field being non-empty, never on level.

### 2.3 The editor was throttling PIE to 3 FPS — a confound that invalidated measurements

Session 2 appeared to stream at ~254 chunks/s against ~2,000. It was not throttled by work:

```
frameMs=333.33   frameMs=333.33   frameMs=333.33
```

**Exactly 1/3 second, repeated — a hard 3 FPS cap**, with all of it in `elsewhereMs` outside
every voxel system while the streaming tick used only 6% of wall. The tick was not slow; it was
only being **called 3 times a second**. Frame numbers in the log confirm it: `[313] → [328] →
[343]` across 5-second gaps.

Cause: the editor's **"Use Less CPU when in Background"** (`bThrottleCPUWhenNotForeground`), on
by default, throttling PIE whenever the editor window loses focus.

**This matters more here than in a normal project: a voxel world streams on the game thread, so
a frame-rate cap IS a streaming cap.** Now set to `False` in BOTH
`Config/DefaultEditorPerProjectUserSettings.ini` and
`Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini` — the `Default*` file only seeds
new users and would not have taken effect alone.

**Nothing measured under this throttle means anything.** Re-measure anything that predates it.

---

## 3. STILL OPEN — what the owner is actually complaining about

### 3.1 Chunk gaps at LOD ring boundaries — NOT diagnosed

Owner-reported repeatedly and **not addressed**. The `airProof` memo changed cost, not the
admission rule that decides which chunks exist at a ring boundary — do not assume it helped.

The owner believes this was solved during the quad era, which makes it a **regression with a
known-good past**. That is the strongest lead available: find when it worked. The ring presets
are `kDefaultRingPresets` (0-128, 128-256, 256-512, 512-1024, 1024-2048, 2048-4096 m); the
parent-admission rule that skips a chunk when its parent ring covers the ground is in
`DispatchJobs` near the `ParentDistSq >= FMath::Square(OuterUU)` test.

### 3.2 Streaming throughput generally

Backlog §0.0. **The "before" number still does not exist** — no measurement of the quad
mesher's chunks/s exists anywhere in the repo. Get it: build `voxel.March 0` +
`voxel.Terrain.RetireQuads 0`, fly the same path, read chunks/s. Producer switches are command
line / ini ONLY; `-ExecCmds` lands after streaming has begun and silently no-ops.

Now that `airProof` is fixed and the throttle is off, **re-measure before doing anything else** —
both prior datasets are contaminated.

### 3.3 GPU mesh jobs time out constantly

**900 timeouts in a focused session, 2,024 in a throttled one:**
`VoxelGpuMesh: job N ... ended TimedOut -- requeueing on the CPU worker path. readback not
ready after 10.0 s`. Every one is a GPU job abandoned after a 10 s wait and redone on the CPU.
Independent of focus. Not investigated. The 10 s timeout is hardcoded at
`VoxelGpuMeshJobManager.h:353`.

### 3.4 Cold fill: the z-range memo waits for residency

`recompute` is 2,815 ms of a 5,000 ms window at cold start, decaying to 85 ms as tiles land.
`FootprintChunkZRangeCached` refuses to memoize while a fine tile is decoding — correct, for
the reason its comment gives. Landed `1e5207b` 2026-08-17, **not marcher code**, costs the quad
path identically. Contributes to initial-load holes.

---

## 4. Other open items

| Item | State |
|---|---|
| GI shows no visible difference | Origin verified CORRECT (`delta (0,0,0)`). Shader path live (`BaseColor *= VoxelGIAmbient`). Volume covers only **±38 m, fades out at 35 m** — the A/B must be done standing on ground within ~20 m. Discriminate with `voxel.GI.DebugVis 1` |
| Marched shadows | Work, but **7-9 ms at 512 m reach** — not the 0.3 ms the 64 m scaffold implied. Dial is `voxel.Shadow.MarchRayReachM` (48/96/256). Reach choice is a designer call, never made |
| Wave 5 — water off the quad pool | Not started. Turns retirement into deletion |
| Depth gate | 27.98% interior disagreement, unexplained |
| Camera-radial shadow segmentation | Not done; per-ray level is the prototype |

---

## 5. Rules that will bite you

- **One Unreal editor per box.** Check with `Get-Process`, never `tasklist` via Bash.
- **A live editor blocks builds** (LNK1104). Builds and legs cannot overlap either way.
- **`.usf` files are live input to the next run.** Stamp with `tools/voxel-stamp-build.ps1`
  only after a SUCCESSFUL build — `&& stamp`, never `; stamp`.
- **`VoxelEarthShaders` may not depend on `VoxelEarth`.** Mirror cvars instead.
- **Adaptive unity hides anon-namespace collisions.** `tools/lint-unity-collisions.py` misses some.
- **Never read a pre-ROP counter as evidence pixels landed.**
- **The owner judges screenshots.** Present captures and conditions, never a verdict.
- **Plain English.** Lead with the effect.

---

## 6. Suggested first move

**Re-measure. Both existing datasets are contaminated** — one by the 3 FPS throttle, one by the
250 ms `airProof` cost that is now gone. Fly a focused PIE session and read chunks/s, tick % of
wall, and where the frame goes.

Then go after **§3.1, the LOD ring gaps**. It is what the owner keeps reporting, it is the one
symptom nothing here has touched, and the owner's belief that it worked during the quad era
gives it a bisection target that the throughput work does not have.

**Do not spend time making the marcher faster.** Section 0 has the numbers: rendering is
8-11 ms against a game-thread tick of 40-79% of wall.
