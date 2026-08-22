# Handoff: GPU ray marcher as primary terrain renderer

**Date:** 2026-08-22
**Branch:** `marcher/wave1-2-shading-and-tick-ceiling` (NOT pushed)
**Working dir:** `D:\voxelsim` (the single checkout — `vox-int` is dead, do not work there)

---

## 1. Where this stands in one paragraph

The marcher renders terrain into the real frame, is **on by default**, and quads are retired
(`retired=1`, `terrainPoolUsedQuads=0`, `residentQuads=0`). Draw-path win over quads is
**2.72x p50 / 2.41x p95** on matched legs. Marched sun shadows and a per-voxel shading pass
(climate tint, surface gradients, voxel GI) are built and default-on. The prototype was opened
in the editor tonight for the first live human look. **It ran, then broke.** Three findings
below, one of them a hard blocker. No further measurement legs were run — the owner called a
halt to measurement a day earlier and wants live editor iteration instead.

---

## 2. How to actually see the thing (this cost an hour tonight — read it)

**This project has NO level asset. Zero `.umap` files.** The voxel world is built entirely in
code by `AVoxelEarthGameMode::BeginPlay()`; `GlobalDefaultGameMode` points at it and
`GameDefaultMap=/Engine/Maps/Entry.Entry`.

Consequences:

- Opening the editor shows an **empty "Untitled" world with default UE landscape**. That is
  correct and expected, not a rendering bug. `BeginPlay` has not run.
- **Terrain only exists in Play-In-Editor (PIE) or `-game`.**
- The spawn defaults to **(0,0)**, which is outside this seed's baked tiles. Every leg used
  `-VoxelSpawnAt=-61440,-61440`. Launch without it and you get an empty world that looks
  exactly like a broken renderer.

Launch line that worked:

```
D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe "D:\voxelsim\ue-project\VoxelEarth.uproject" -dx12 -sm6 -VoxelSpawnAt=-61440,-61440 -VoxelNoClipmap
```

Then press Play. Allow 30-60 s to settle before judging anything.

---

## 3. FIXED 2026-08-22 — the second PIE session rendered an empty sky

> **STATUS: fixed and verified. Read this for the diagnosis, not for open work.**
> The gate reports `Voxel GPU teardown: released 71060 pool chunks and 71060 index
> entries (pool now 0, index now 0)` and session two now streams.
>
> **It did NOT fix the owner's experience.** Session two still fills at roughly a
> seventh of session one's rate, and the owner reports streaming is worse than the
> pre-marcher quad system generally. **That is the real problem and it is now
> §0.0 in `docs/backlog.md` — start there, not here.**
>
> There were **three** links, not one, and the third is the reusable lesson:
> `TArray::SetNumZeroed` only zeroes elements it ADDS, so it was a silent no-op on
> re-attach and the 4 MiB index grid kept every stale bit. Both teardown
> primitives — `FVoxelBrickPool::Reset` and `FVoxelMarchChunkIndex::Detach` —
> already existed with **zero callers**.

**Symptom (owner-observed):** first PIE session showed terrain. Pressed ESC, pressed Play
again — sky only, no terrain ever streamed in.

**Confirmed in the log, not inferred.** `Saved/Logs/VoxelEarth.log`:

| Moment | line | reading |
|---|---|---|
| PIE #1, healthy | 67796 | `resident0=43338 indexEntries=120769 released=11542 evictions=0` |
| PIE #2, onset | 128324 | `resident0=51415 indexEntries=131047 released=0 evictions=1227` |
| PIE #2, later | 133911 | `resident0=53519 indexEntries=131047 released=0 evictions=59954` |
| shadow march | 129034 | `NO RAYS MARCHED over 600 retired frames ... sky=1027408 far=0 backface=0` |

Read those columns together:

1. **`indexEntries` is stuck at 131,047 against a pool capacity of 131,072.** The chunk index
   is essentially full, and it **carried over from PIE #1** — it never fell when the first
   session ended.
2. **`released` is 0 for the whole second session** — but was 11,542 in the first. The Wave 1.2
   brick-release path works; it has nothing to release, because the pool's contents are orphans
   from the previous session that no live chunk record owns.
3. **`evictions` climbs ~200 per 5 s** from zero. A full pool evicts farthest-from-focus, which
   is precisely what "nothing ever streams in" looks like.
4. `sky=1027408` of `considered=1027408` — every pixel is sky. Confirms the frame is genuinely
   empty rather than mis-shaded.

**Diagnosis:** the brick pool and the march chunk index are **not torn down when a PIE session
ends**. They outlive the world, so session two starts against a saturated index full of dead
chunks and can never admit a new one.

**Where to look:** the pool is a process-lifetime object in `VoxelEarthShaders`, while the
records that own its contents live on `UVoxelWorldSubsystem` in `VoxelEarth`, which dies with
the world. Nothing bridges that gap. `UVoxelWorldSubsystem::Deinitialize` should release
everything it put in the pool, and/or the pool needs an explicit world-teardown reset.

**The asymmetry that made this easy to miss:** every leg ever run was a fresh `-game` process,
so the pool was always empty at start. **PIE re-entry is a code path no measurement has ever
exercised.** This is a pre-existing latent bug that the prototype merely revealed.

`writesDropped=33` also appears only in session two — same cause; confirm it returns to zero
once teardown is fixed.

---

## 4. GI showed no visible difference — cause NOT established

**Owner-observed:** `voxel.GI.Enabled 0` vs `1` looked identical.

Two things are already ruled out:

- **The origin is correct.** `GI ORIGIN AUDIT: pool 'VoxelTerrainPool' ... delta (0,0,0) = 0 UU.
  SAME SPACE` (log:2375). The origin bug predicted in
  `voxelsim-marcher-flip-silent-regressions` does **not** apply to terrain.
- **The shader path is structurally right.** `VoxelMarch.usf:1061-1105` samples three probes and
  applies `BaseColor *= VoxelGIAmbient` at `:1108`. Not dead code, not a neutral write.

Two candidates remain, in my order of likelihood:

**(a) The camera was outside the GI volume — a benign non-result.** Log:2376-2378 report
`coverage=+/-3840 UU` (±38.4 m) and `fade=1920..3520 UU`. **GI is fully faded out beyond 35.2 m
of the field centre.** If the owner was in flycam at altitude or looking at mid-distance
terrain, zero difference is the *correct* behaviour. The A/B must be done **standing on the
ground, under an overhang, within ~20 m.**

**(b) The GI producer has no input now that quads are retired.** `VoxelGI.cpp:533` and `:1280`
take the volume's anchor from `Terrain->GetTerrainGpuPool()` — the pool that retirement
deliberately empties. If the compute pass that *fills* the volume also reads that pool, the
volume is uniform and toggling it changes nothing. **This is a hypothesis, not a finding — I
did not verify what feeds the volume's contents.** Check the producer in `VoxelGIMarchPass.cpp`.

**Cheapest discriminator:** `voxel.GI.DebugVis 1` (the cvar exists; log:2378 reports
`debugVis=0`) and look at whether the volume has any structure in it at all. That separates (a)
from (b) in one look.

---

## 5. Shadows worked, and have a real cost

Marched sun shadows ran in mode 2 and produced sensible censuses (log:109351, 114272, 125569):
`hit%` 55.7 / 48.8 / 15.2 across three poses, `exhausted/f=0.00`, all decline counters zero.

**But `gpuMs mean=6.981 / 7.173 / 9.209` at `reach=512.0m`.** That is expensive — the quad
path's entire ShadowDepths pass was 15.8 ms, so this is not the bargain the earlier
0.335-0.409 ms figure at 64 m suggested. `voxel.Shadow.MarchRayReachM` is the dial (try
48 / 96 / 256); the cost sweep was never run.

**Unresolved: what reach actually looks good enough.** That is a designer question, and
answering it was the point of opening the editor.

---

## 6. What is committed, and the 74 dirty files

Two commits on the branch:

- `9037b54` — Wave 1.2 brick lifetime, Wave 2 shading data format, and the streaming-tick ceiling
- `8d884de` — the prototype: marched terrain on by default, with shadows, GI and climate

**Nothing is pushed.** `719d2b2` (voxel-core cover producer) is also unpushed.

`git status` shows **74 dirty: 35 modified, 39 untracked.** They are neither junk nor reviewed.
Roughly: ~10 modified docs (`status.md`, `backlog.md`, the roadmap and waves plans), config
(`DefaultEngine.ini`, `DefaultGame.ini`), a spread of `VoxelEarth` sources, and ~28 never-tracked
source files. **These need an audit before the branch is pushed** — the owner asked about them a
day ago and the audit never happened.

Also outstanding: extend `.gitignore` to cover `bake-out/karst/`.

---

## 7. The tick-ceiling win (already landed — do not re-derive)

`MarkDirtyAndUpload()` ran an FNV-1a hash over the whole 4 MiB chunk-index grid on the **game
thread**, once per flush, to serve `voxel.March.VerifySource` — a diagnostic that defaults to 0
and never arms. Now gated behind `voxel.March.IndexContentHash` (default 0).

Same-binary A/B: flight mean frame **50.18 → 45.51 ms (−9.3%)**, tick share **93.5% → 40.5%**,
throughput **1,743-2,001 → 2,274-2,435 chunks/s**.

---

## 8. Waves still open

| Wave | State |
|---|---|
| 1.1 fluid decoupling | **Done** |
| 1.2 brick lifetime | **Done** — but see the PIE teardown blocker in §3 |
| 1.2b load-before-unload | **Reverted deliberately.** Correct diagnosis, bad trade: evictions 0 → 151,657, pool to 96%, p95 +25%, bought only 5-10% of holes |
| 2 per-voxel shading | Built (climate, gradients, GI). **Unjudged by a human** |
| 3 marched shadows | Built and working. **Cost sweep and reach choice outstanding** |
| 4 flip the default | **Done** — all defaults flipped, no command-line args needed |
| 5 water off the quad pool | **Not started.** This is what turns retirement into deletion |

Also worth queueing: camera-radial shadow segmentation (per-ray level is the current
prototype); confirming water's GI origin bug at a lake; the depth gate's unexplained 27.98%
interior disagreement.

---

## 9. Rules that will bite you

- **One Unreal editor per box.** Check with `Get-Process`, never `tasklist` via Bash — tasklist
  reports "all clear" while an editor runs.
- **A live editor blocks builds** (LNK1104 on the DLLs). Builds and legs cannot overlap in
  either direction. This bit twice.
- **`.usf` files are live input to the next run.** Stamp with `tools/voxel-stamp-build.ps1` only
  after a SUCCESSFUL build, and it is `&& stamp`, never `; stamp`.
- **`VoxelEarthShaders` may not depend on `VoxelEarth`.** Mirror cvars instead.
- **Adaptive unity hides anon-namespace collisions.** `tools/lint-unity-collisions.py` catches
  some, not all — it missed one of two pairs this session.
- **Never read a pre-ROP counter as evidence pixels landed.** `tiles`, `emitFrames`, `marchMs`
  and every decline counter describe work *before* raster output. A blank frame with perfect
  counters is exactly what two of this feature's bugs looked like — and what §3 looks like.
- **The owner judges screenshots.** Present captures and conditions, never a verdict.
- **Plain English.** Lead with the effect.

---

## 10. Suggested first move

**`docs/backlog.md` §0.0 — the streaming regression.** The owner flew the prototype and
reported that streaming is **slower than the quad mesher the marcher replaced**, with chunks
missing at LOD boundaries and holes opening up when flying at normal speed.

That outranks everything else in this document, including the 2.72x draw-path win. **A
renderer that draws an incomplete world quickly is not faster**, and every marcher-vs-quad
measurement taken so far reads frame time rather than world completeness, so none of them
could have caught it.

The first move is a comparison, not a hunt: build the pre-marcher quad configuration
(`voxel.March 0`, `voxel.Terrain.RetireQuads 0`), fly the same path, and read chunks/s and the
LOD-boundary behaviour. That says whether this is the marcher's producer path or something
underneath both arms. **The producer switches are command line / ini only** — `-ExecCmds`
lands after streaming has begun and silently will not apply.

This is the one place a measurement leg is still the right tool: the owner's "stop measuring"
direction was about re-litigating the draw path, and this is a different question that a human
in the editor has already answered qualitatively. What is missing is the *before* number.
