# Voxel debug mode, profiler & diagnostics (working plan)

Purpose: make terrain (and later water) performance and streaming behavior
VISIBLE — to designers in the viewport, to engineers in stats/traces, and to
agents/CI through captures and CSV. One toggle, layered depth.

## Access model

| Surface | Mechanism |
|---|---|
| Master toggle | `voxel.Debug` cvar: 0=off, 1=perf HUD, 2=HUD+visualizations. F3 cycles in PIE/game. |
| Layer toggles | `voxel.Debug.Rings`, `voxel.Debug.ChunkStates`, `voxel.Debug.Bounds`, `voxel.Debug.Water` (future) — independent, all live under mode 2. |
| Engine stats | `stat VoxelEarth` group (DECLARE_CYCLE_STAT/COUNTER) — visible in `stat` HUD, Unreal Insights traces, Session Frontend. |
| Headless/CI | FCsvProfiler custom category `VoxelEarth` — scripted runs emit CSV; a checker script asserts thresholds (this becomes the M1 60fps and M2 no-hitch gate harness). |
| Agent/MCP | All of the above work in PIE + `-game`; debug visualizations + CaptureViewport = agent-readable diagnostics; log categories flippable at runtime via the editor MCP LogsToolset. |

## Visualization layers (mode 2)

1. **LOD ring color-coding** (the designer-facing headline): every chunk
   component gets a debug tint by mip level — R0 green, R1 yellow, R2
   orange, R3 red, R4 magenta, heightmap band cyan (palette in one constant
   table; revisit for colorblind safety). Implemented as a multiply-tint
   parameter on M_VoxelTerrain driven per-component (MID) so AO/shape stays
   readable; `voxel.Debug.RingsOpaque 1` swaps to flat unlit colors when
   maximum contrast beats readability. Activates fully with M2 rings; in M1
   everything is R0/green.
2. **Chunk state overlay**: tint pulse by state — just-loaded (blue flash,
   1s decay), edited/overlay-aware chunks (orange), game-thread re-meshed
   this frame (purple flash), pending-unload (grey). Makes streaming churn
   and edit invalidation literally visible.
3. **Bounds/wireframe**: chunk AABB lines (batched line draws) for the
   chunks in view; ring boundary shells drawn as translucent bands at the
   R0/R1/... radii around the anchor.
4. **Water (future, same framework)**: active CA region boxes with per-brick
   tick-age heatmap, reservoir surfaces with level+volume labels, SWE patch
   bounds, force-field vector arrows at ~1m grid, volume-ledger HUD line
   (conservation delta MUST read 0 — a visible nonzero is a bug alarm).

## Perf HUD (mode 1)

Canvas panel (extends the M1 HUD), ~12 lines, 1Hz refresh with per-frame
sparklines for the hot rows:

- **Streaming**: loaded/unloaded chunks (cumulative + per-second), jobs in
  flight vs cap, queue depths (worker/game-thread/unload), budget saturation
  (% of per-frame apply/unload/re-mesh budgets used), stale results
  discarded.
- **Worker timings**: rolling histogram of per-chunk gen-ms and mesh-ms
  (p50/p95/max) — the number that caught the 5-chunks/s bug becomes a
  first-class metric.
- **Memory**: resident chunk components, total quads, vertex+index buffer MB
  (pooled buffer stats later), overlay brick count, edit-log entries/bytes.
- **Frame**: game/render/GPU ms (engine values) + our slice: subsystem tick
  ms, edit re-mesh ms this frame.
- **Water (future)**: active CA bricks, CA ticks/s, SWE patches, volume
  ledger in/out/delta, replication KB/s.

## Instrumentation plumbing

- **voxel-core stays engine-free**: a plain `vxc::Counters` struct (atomics,
  header-only) collected by World/mesher/editlog call sites; the UE layer
  samples it and republishes to stats/CSV. No UE types below the boundary.
- **UE layer**: SCOPE_CYCLE_COUNTER on subsystem tick, worker job body,
  edit re-mesh, proxy build; TRACE_BOOKMARK on ring transitions and
  explosive detonations (Insights navigation anchors).
- **Log hygiene**: split LogVoxelEarth into LogVoxelStream, LogVoxelEdit,
  LogVoxelPerf, LogVoxelWater (future); default Display, Verbose behind the
  debug mode; periodic counter lines move to LogVoxelPerf.

## Regression harness (the gates, automated)

`-VoxelPerfRun=<seconds>` command line: scripted flight (fixed seed, fixed
spline over the anchor at M2 speeds) + CSV capture + end-of-run JSON summary
(p95 frame ms, hitch count >33ms, budget saturation, chunks/s). A checker
script (tools/check-perf-run.py) asserts thresholds and exits nonzero — CI
runnable on this machine, and the artifact IS the M1/M2 gate evidence.

## Phasing

- **P1 (next wave, after current pawn/edit-tools wave merges — touches the
  same subsystem files)**: cvars + F3, stats group, vxc::Counters, perf HUD,
  chunk-state tints, bounds layer, log split, -VoxelPerfRun + checker.
- **P2 (with M2 rings)**: ring palette activation, ring-boundary shells,
  per-level HUD rows, flight-run thresholds tightened to the M2 gate.
- **P3 (with W2 water)**: water layers + volume-conservation ledger line.
