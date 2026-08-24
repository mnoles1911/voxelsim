# Splitting the render frame — handoff, 2026-08-24

## Why this lane exists

`VoxelFramePhase` established that this project is **RENDERBOUND at every speed
and in both motion states**:

    seg                    gameBusy  renderBusy  frame  floorFps  bound
    M20 SETTLED-PARKED        1.69       9.23     9.32     108    RENDERBOUND
    M20 SETTLED-MOVING       12.20      18.60    26.14      54    RENDERBOUND
    M30 SETTLED-PARKED        1.87      10.12    10.25      99    RENDERBOUND
    M30 SETTLED-MOVING       11.35      17.47    28.24      57    RENDERBOUND

Parked, the render thread alone is a 108 fps ceiling with nothing streaming.
Moving, it is a 54 fps floor — **deleting 100% of the remaining game-thread work
cannot reach 100 fps.** And `renderBusy` was one number. Nothing in this project
had ever split it.

## What `renderBusy` actually is — read from the engine, not assumed

`SlateRHIRenderer.cpp:1356` (UE 5.8), at Present:

    uint32 ThreadTime = EndTime - LastTimestamp;          // present-to-present
    RenderThreadIdle  = FThreadIdleStats::Waits
                      + GRenderThreadIdle[WaitingForGPUQuery]
                      + GRenderThreadIdle[WaitingForGPUPresent];
    GRenderThreadTime     = ThreadTime - RenderThreadIdle;
    GRenderThreadWaitTime = RenderThreadIdle;

So **18.60 ms is render-thread CPU work with all registered idle already
removed.** It is not GPU time and it is not a GPU wait.

This matters because the only render-side timing this project already had —
`VoxelMarch.TimeBegin/TimeEnd`, the `OpenBracket`/`CloseBracket` ring in
`VoxelMarchRenderer.cpp` — issues `RQT_AbsoluteTime` **GPU** queries. It measures
a different axis entirely. It can read near zero while this reads 18.60 and
neither is wrong. **Do not quote one against the other.**

## The split

Anchors, read from the engine's own call sites:

| anchor | site | what it is |
|---|---|---|
| A | `SceneRendering.cpp:4299` `PreRenderViewFamily_RenderThread` | start of `FSceneRenderer::Render` |
| B | `SceneRendering.cpp:4956` `PostRenderViewFamily_RenderThread` | end of `Render` — still graph CONSTRUCTION |
| E | `RenderGraphBuilder.cpp:2214` post-execute callback | after `GraphBuilder.Execute()` (`SceneRenderBuilder.cpp:916`) and after its parallel-translate await |

    setupMs    A -> B    scene-renderer graph construction: visibility, GPU-scene
                         update, mesh draw command setup, every AddPass call
    executeMs  B -> E    RDG execute: recording RHI commands
    tailMs     E -> A'   EVERYTHING ELSE THE RENDER THREAD DID — RDG flush and
                         cleanup, Slate/UI draw, Present enqueue, every other
                         ENQUEUE_RENDER_COMMAND (brick-pool uploads, chunk-index
                         uploads, GI volume uploads, mesh-job dispatch/poll/fetch,
                         readback polls), and the next frame's scene update
                         before the first extension hook.

Inside `setupMs`, the extensions this workstream owns are timed by name:
`mFam mView mBase mEmit` (marcher), `fluid`, `shadow`, and `setupOther` as the
explicit residual.

**Every bucket is BUSY, not wall.** Each anchor samples the same two counters
the engine sums at Present, so each bucket subtracts its own idle and the three
compare directly to `GRenderThreadTime`.

**The reconciliation delta is printed**, in absolute ms and as a share, and the
line says `recon=INVALID` when `|delta| > 15%` of `renderBusy`. `setupOther` is
allowed to print negative; `IdleTail` is deliberately not clamped at zero. A
bucket that cannot go negative is not a measurement.

## Registered disproof — written before the leg

- **D0 the instrument is valid.** Disproved if `|reconDelta| > 15%` of
  `renderBusy` in SETTLED-MOVING, or `families/frame > 1.01`, or
  `renderBusyMs = 0.00`. If D0 fails nothing else may be quoted.
- **D1 the delta is the marcher's own passes.** Disproved if
  `d(mFam+mView+mBase+mEmit) < 20%` of `d(renderBusy)`.
  **I expect this to be disproved and am writing it down first.** The marcher's
  hooks add a fixed handful of passes whose count does not depend on how many
  chunks are resident. If they measure ~0.2 ms in both segments then every "make
  the marcher cheaper" proposal is aimed at a bucket that is not moving, and the
  instrument will have earned its cost by killing those proposals.
- **D2 scene-renderer setup.** Disproved if `d(setupOther) < 20%` of `d(renderBusy)`.
- **D3 RDG execute.** Disproved if `d(execute) < 20%` of `d(renderBusy)`.
- **D4 outside the scene renderer.** Confirmed if `d(tail) > 50%` of `d(renderBusy)`.
  Then the cost is other render commands / Slate / present-adjacent work, this
  lane names nothing further from that leg, and it moves to whoever owns those
  enqueues.

**D1–D4 can all be disproved at once.** That is a legitimate result and must be
reported as "the render frame does not respond to motion in any bucket this
instrument can see", not resolved by picking the largest.

## The mutation arms — none of the six existing arms in this codebase has ever been run

`-VoxelRenderFrameMutate=N -VoxelRenderFrameMutateMs=X` (default 2.0):

| arm | injects | expected | RED means |
|---|---|---|---|
| 1 | burn X ms in `PreRenderBasePass` | `mBase` +X, `setupOther` unchanged, `renderBusy` +X | it landed in `setupOther`; the scopes are not where they claim |
| 2 | burn X ms in the post-execute callback | `tail` +X | it landed in `execute`; anchor E is misplaced |
| 3 | **BLOCK** X ms in `PreRenderBasePass` | `mBase` busy UNCHANGED, `sveBlocked` +X, `renderWait` +X, `renderBusy` UNCHANGED | `mBase` busy rose by X — every bucket is wall time wearing a busy label and the reconciliation is a coincidence |
| 4 | burn X ms inside an RDG pass lambda | `execute` +X | it landed in `setup`; RDG did not defer |

**Run arm 3 first.** It is the only one whose failure invalidates the whole file,
and it is cheap. Arm 4's RDG pass is added **only when arm 4 is selected** — an
ordinary `-VoxelRenderFrame=1` leg adds no pass, so the measured build and the
control do not differ by one (`-VoxelFineLockMeter=1` cost 5% of a cold start
for exactly that class of mistake).

## The legs

Matched to `G10-M20` / `G10-M30`, which produced the 9.23 / 18.60 figures — same
spawn, same flight, same flags, plus `-VoxelRenderFrame=1`:

    # M20, the gate speed
    tools\voxel-run-flight-leg.ps1 -LogName rf-M20 `
      -SpawnAt "-61440,-61440" -Flight line -RunSec 120 -PreflightSec 90 `
      -LingerSec 60 -LogIntervalSec 2 `
      -Cvars "voxel.Debug 0, voxel.Stream.CoverageVerify 1" `
      -ExtraArgs @('-VoxelGpuPrimary=1','-VoxelGpuPoolAlloc=1','-VoxelGpuWorldGenBatch=1',
                   '-VoxelGpuStackClaim=1','-VoxelGpuRasterAtlas=1',
                   '-VoxelFramePhase=3','-VoxelPerfSpeed=20','-VoxelRenderFrame=1')

    # the negative control -- run this one FIRST
    ... same, -LogName rf-mut3, plus '-VoxelRenderFrameMutate=3','-VoxelRenderFrameMutateMs=2'

    # M30, so the curve is visible rather than a single point
    ... same, -LogName rf-M30, -VoxelPerfSpeed=30

Read with `tools/read-render-frame.sh Saved/rf-M20.log`, which prints in the
order that stops the wrong-window mistake and cross-checks `renderBusyMs`
against the frame-phase line on the same leg.

**Resolution.** `G10-M20` ran `-ResX=1600 -ResY=900` while the owner's target is
2560x1440. Render-thread CPU is largely resolution-independent, so the two are
comparable — but any arm that claims a frame-rate result must state its
resolution, and a matched pair must not straddle one.

## Failing readings, both ways

- no `Voxel render frame` line with `-VoxelRenderFrame=1` → hooks not applied,
  or neither the marcher nor the fluid extension registered
- `frames=0` in a segment → that population is EMPTY; a PARKED line may never be
  quoted in a MOVING line's place
- `recon=INVALID` → the buckets do not describe the frame; quoting one is the
  same error as a percentage without its denominator
- `families/frame > 1.01` → `setupMs` swallowed an intermediate Execute; the
  three-way split is not a partition
- `renderBusyMs=0.00` → HARD ZERO, the reconciliation target is dead; it does
  **not** mean the render thread is idle
- `dropped > 0` → frames where A fired and E never did, discarded rather than
  reconstructed
- `camSpeedMS ~ 0` on a MOVING line → the segmenter is wrong; invalid leg, not a
  fast one
- every bucket EQUAL between MOVING and PARKED → the render thread does not
  respond to motion anywhere this file can see. Say that; do not pick the largest
- **`shadowMs=0.000` is the CORRECT reading on a stock leg** and is named inline
  in the log text. `voxel.Shadow.March` defaults 0 by owner decision since
  2026-08-23, the extension declines `IsActiveThisFrame`, and not one hook is
  called. It is this file's own named dead reading and is **not** evidence that
  shadows are free.

## What is already established without a leg

- **The marcher does not cast sun shadows, and that is a deliberate default, not
  a silent gap.** `voxel.Shadow.March` is `0` with the cvar text recording the
  owner's decision and calling it "the largest single frame-time item". The
  recorded 15.8 ms terrain-shadow figure belongs to the quad path, which the
  marcher replaced. So a large render cost today is **not** terrain shadows.
- **Terrain is not in the scene as primitives under the marcher.** Records hold a
  pool slot rather than a component (`HoldsGeometry` / `HoldsTerrain`), so
  primitive add/remove and GPU-scene churn from terrain are not the moving-vs-
  parked mechanism. That removes the most obvious candidate before the leg runs.
- **There is substantial per-frame render-command traffic outside the scene
  renderer**, and all of it lands in `tail`: `VoxelGpuMeshDispatchBatch` /
  `VoxelGpuMeshPoll` / `VoxelGpuMeshFetchQuads` / `VoxelGpuMeshCompactQuads`,
  `VoxelBrickPoolFlush` / `VoxelBrickPoolAllocWindow`, the chunk index's three
  enqueues, residency's three, the GI volume's nine. Every one of them scales
  with streaming, and streaming is what motion causes. **That is the prior for
  D4, and it is stated before the measurement rather than after it.**

## The hooks this lane does not own

If the leg confirms D4, attributing `tail` further needs one
`VOXEL_RENDER_FRAME_SCOPE` inside the lambda of each `ENQUEUE_RENDER_COMMAND`
listed above, in files owned by other agents. That is a one-line change per
site and needs a new `EBucket` per group. Nothing else in this instrument
changes.
