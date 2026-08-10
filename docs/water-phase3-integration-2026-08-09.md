# Phase 3 integration pass — collision live + the faucet/sink lifecycle v1

> **STATUS 2026-08-09: CURRENT.** The integration pass that wired the three
> parallel builds (fluid spikes `66f0619`, scalar hydrology `ebcd6c0`, basin
> v2 `88e83c1`) into one lifecycle. Companion to
> `docs/water-rearchitecture-plan-2026-08-09.md` (Phase 3 row) and
> `docs/water-architecture.md` §3. The measurement script at the bottom is the
> integrator's; captures are judged by the owner, not by the implementer.

## What is now wired

1. **Collision is live.** `VOXEL_FLUID_HAS_COLLISION=1` is baked into every
   solver compile (`FVoxelFluidShader::ModifyCompilationEnvironment`). The
   per-iteration Δp correction projects against the occupancy volume, and
   `FluidFinalizeMain` walks the FULL step (stored position → constrained
   position) through `VoxelFluidResolveCollisionEx`, lifting the previous
   position onto the contact plane so the derived velocity has exactly zero
   approach speed (contract items 2/5/7 — the ratified recipe is now the
   implementation). The anti-tunnelling guarantee lives in that finalize walk:
   a prediction that crossed a thin wall during integrate is caught from the
   known-free start-of-step position.
2. **The occupancy volume is owned and fed.** `UVoxelFluidSubsystem` owns the
   `FVoxelFluidOccupancyVolume`; the origin latches as the volume's min corner
   centred on the camera (contract item 1: `FluidOriginUU == origin voxel ×
   10`; positions live in `[0, 5120]` UU; boundary box centre is uploaded, no
   longer origin-centred). Initial fill: 512 regions of 64³ voxels, queued
   centre-out, packed on the game thread from the overlay-aware
   `IsSolidAtVoxel` under a time+count budget
   (`voxel.Fluid.Occupancy.RegionsPerTick` / `.PackMsPerTick`). Terrain edits
   and arrivals reach the volume through
   `UVoxelWorldSubsystem::SetFluidTerrainDirtyListener`, fired from the two
   documented call sites (`MarkChunkDirtyForRemesh`, level-0 keys only, before
   its untracked early-return; `ApplyMeshResult`, level 0).
3. **Ordering is enforced, not trusted.** `AddPasses` runs in the SAME
   `FRDGBuilder` before any solver pass; a `checkf` against the volume's new
   `FStats::AddPassesCount` fails any refactor that moves it. A tick without a
   usable volume is SKIPPED and counted (`skippedNoOcc` in the perf line) —
   with the define baked on there is no silent no-collision path.
4. **The verify gate exists** (`voxel.Fluid.Occupancy.Verify 1`): 16 MiB
   readback, one 64³ region byte-compared per landed snapshot (rotating
   cursor) against `vxc::fluidFillRegion` — the unit-tested CPU reference the
   GPU kernel hand-mirrors. Results: `verify=pass|FAIL|stale|off` in the perf
   line; every mismatch logs its first differing word with world coordinates.
   `stale` = an edit landed between snapshot and compare (skipped, counted).
5. **The lifecycle v1** (`voxel.Fluid.Faucets 1`):
   - **Headwater faucets**: `UVoxelWaterSubsystem::GatherHeadwaterFaucets` —
     baked `SECTION_HEADWATERS` (`FineTile::heads()`, bv24) when resident,
     else the bv23 fallback (`RiverNetwork::buildFromBakedWater` over the
     active box, `headwaterNodes()`; Q unknown there → cvar
     `voxel.Fluid.Faucets.DefaultQ`, default 8e6 m³/yr ≈ 253/s). Emission is
     scheduled by an exact integer accumulator (below), under the shared
     budget `voxel.Fluid.MaxSpawnPerTick`; overflow is re-carried, never
     dropped.
   - **Basin sink v1**: one basin (stated limit) — nearest `holdsWater()`
     basin whose bbox intersects the active region
     (`vxc::fluidPickBasinSink`, unit-tested), its clipped bbox + LIVE
     ledger-adjusted datum uploaded as finalize uniforms; particles inside
     below datum despawn `DESPAWN_BASIN` and the CPU credits
     `CreditBasinVolume` at exactly **255 units per particle**. Refused
     credits (tile streamed out) stay pending and retry.
   - **Boundary sink v1**: `DESPAWN_BOUNDARY` counts → 255 units each →
     `InjectRiverInflowNearVoxel`, attributed to the segment nearest the
     volume centre (positions are not read back — stated v1 limit). No graph
     in reach → units stay pending and retry; teardown records any final
     remainder as LOST, loudly.
   - **Sill faucets**: `UVoxelWaterSubsystem` holds spill events whose baked
     outlet falls inside the fluid's registered intercept box; the fluid
     drains them and emits `floor(units/255)` particles at the outlet;
     sub-particle tails and anything unemitted at teardown are refunded
     (`refundSpill`). Events outside the box route through the graph exactly
     as Phase 2 shipped; a grace-window flush (10 s) refunds anything a dead
     fluid host never drained.
6. **Conservation, extended across the seam.** The GPU-side invariant
   (spawned − despawned == alive) is unchanged; every readback additionally
   reconciles, in ledger units:
   `basinDespawns×255 == credited + pending + lost`,
   `boundaryDespawns×255 == injected + pending + lost`,
   `spillClaimed == spillEmitted×255 + spillRefunded + outstanding`.
   Violations are counted and logged (`scalarViolations`).

## The unit conversion, derived once

```
1 particle == one 10 cm voxel of water at rest spacing   (contract :27-29)
one 10 cm voxel == (100 mm)^3 = 1,000,000 mm^3 == exactly 1 litre
one ledger unit == 1/255 voxel                            (basinledger.h "THE UNIT")
⇒ 1 particle == 1 L == 255 ledger units
```

The constant is `vxc::kFluidLedgerUnitsPerParticle` in
`voxel-core/include/voxelcore/fluidlifecycle.h`, **defined from**
`kBasinLedgerUnitsPerVoxel` (never a literal), with `static_assert`s pinning
the chain and `test_fluidlifecycle.cpp` asserting both classic factor-of-255
errors as inequalities.

Faucet rate: `particles/s = Q[m³/yr] × 1000 / 31,557,600` (Julian year).
8e6 m³/yr → 253.5/s (the plan's worked example, asserted by test). The
schedule is the integer accumulator `vxc::FluidFaucetAccumulator` (carry in
m³·µs/yr; one particle costs `31,557,600 × 1000`), drift-free over hours by
test.

## Deviations / stated limits

- ~~**No recentring (v0)**: the origin latches once; toggle
  `voxel.Fluid.Enable` to re-anchor.~~ **SUPERSEDED 2026-08-10** — the window
  now follows the camera (`UVoxelFluidSubsystem::MaybeRecentre`); the toggle
  workaround is gone. See the addendum "The recentring policy is wired" at the
  bottom of this file.
- **One basin sink per frame** — a region straddling two lakes credits only
  the picked one; v2 is a table upload (contract item 6 says so in place).
- **Boundary attribution** is by region centre, not exit position (positions
  aren't read back). Counted, documented.
- **Fallback headwaters** (bv23): rim of the box can carry false heads
  (rivernet.h's own caveat), and Q is the DefaultQ cvar, not baked. Both
  vanish when the running bake lands bv24 heads.
- **Occupancy solidity** goes through boolean `IsSolidAtVoxel` (the exposed
  overlay-aware path). Under `-VoxelWaterMarker=1` the marker's solid voxels
  would wall off marked rivers — the marker is a bring-up diagnostic and the
  fluid is not expected to run in that mode (noted at the pack site).
- **Initial fill is multi-second by design** (generation-bound, budgeted);
  unfilled space is SOLID, so early particles freeze rather than leak — the
  perf line's `occupancy=<built>/<deferred>` shows progress and the fill
  logs completion.
- Headwater faucet water is **exogenous** (created from baked Q, like graph
  baseflow); it is not debited from any ledger. Sill-faucet water IS ledger
  water and is fully reconciled.

## Perf line (1 Hz), all fields distinguishable from did-not-run

```
Fluid perf [run] alive=… spawned=… requested=… despawnBasin=… despawnBoundary=…
  simGpuMs=… iters=… slots=… violations=…
  faucet=<n>/s(n=<count>,heads|fallback)|off|gathering  spill=<n>/s
  sink(basin)=<n>/s|none|off  sink(boundary)=<n>/s
  occupancy=<regionsBuilt>/<deferred>  verify=pass|FAIL|stale|pending|off
  skippedNoOcc=<n>
  recentre=<n>(cells=…,rebase=…/… slots,miss=…,stale=…,tele=…,refused=…,checks=…)|off
Fluid ledger: emittedFaucet=… emittedSpill=… creditedToBasin=…(+… pending)
  injectedToGraph=…(+… pending) spillClaimed=… spillRefunded=… scalarViolations=…
```

## In-editor measurement script (integrator)

All PIE/game, one editor per box. Conditions + captures per standing rules;
no verdicts here.

**A. Dam break vs terrain (collision live)**
```
voxel.Fluid.Enable 1
voxel.Fluid.DebugDraw 1
; wait for "initial fill COMPLETE" in the log (occupancy=512/0), then:
voxel.Fluid.Spawn 5000
```
Read: particles pool ON the ground/slopes instead of the old infinite plane;
`Fluid perf` line `simGpuMs`, `violations=0`, `skippedNoOcc=0`. Dig a trench
under the pool (`TryDig`) — water should follow the edit within the region
budget. 90 s same-pose streaming guard applies if anything looks off.

**B. Occupancy verify gate**
```
voxel.Fluid.Occupancy.Verify 1
```
Read: `verify=pass` cycling (one region per snapshot; 512 regions ≈ full
volume sweep). Any `FAIL` line carries the first mismatching word + world
voxel. Turn off after — it costs a 16 MiB readback per cycle.

**C. Faucet river filling a basin (the lifecycle loop)**
Fly to a spot where a baked river reaches a lake (lake-survey sites; verify
the site per the site-verification tools first). Then:
```
voxel.Fluid.Enable 1
voxel.Fluid.Faucets 1
voxel.Fluid.DebugDraw 1
```
Read, in order: `faucet=<n>/s(n=…,heads|fallback)` non-zero → stream of
particles at the head(s); `sink(basin)=<n>/s` non-zero as particles reach the
lake; `Fluid ledger: creditedToBasin=…` climbing;
`voxel.Water.CreditBasin`-style verification via `GetBasinLedgerStats`
(sumUnits climbing) and the LAKE SHEET rising over minutes (the sink datum is
the live ledger-adjusted level, so the rise feeds back into where particles
despawn). At 253/s ≈ 65 kL/h — for a visible rise in minutes on a small
basin, raise `voxel.Fluid.Faucets.DefaultQ` (e.g. 8e8 ≈ 25k/s, budget-capped
by `voxel.Fluid.MaxSpawnPerTick`) and say so in the conditions.
- The scalar-only cross-check (no particles): `voxel.Water.CreditBasin
  <units>` still works and should agree with the sheet behaviour.

**D. Sill faucet + boundary sink**
With C running and the lake credited to its sill (`GetBasinLedgerStats`
spilled units > 0): spill events whose outlet is inside the region emit as
particles at the saddle (`spill=<n>/s`); events outside route/refund as
Phase 2 (`voxel.Water.Rivers 1` to give them a graph). Boundary:
`sink(boundary)=<n>/s` with `injectedToGraph` climbing when a graph is armed;
with no graph, `+pending` holds the units (nothing dropped) — that is the
designed state, not a leak.

**E. Conservation soak**
Leave C running 10+ minutes: `violations=0`, `scalarViolations=0`,
`spillRefunded + emitted` closing against `spillClaimed`, then
`voxel.Fluid.Enable 0` and read the teardown line (refunds + any stated LOST
units).

---

## Addendum: spike (b) / Phase 4 — the screen-space fluid renderer (2026-08-09)

Appended by the renderer build; everything above is the integrator's and is
unchanged. This section records what was built, the compositing decision and
its evidence, the offline verification, and the measurement runs the
integrator should do (captures judged by the owner, standing rule).

### What was built

`voxel.Fluid.Render 1` (default **0** — off; `DebugDraw` points remain the
fallback view) draws the alive particles as a smooth fluid surface:
depth splat → separable bilateral smooth → normals → Beer–Lambert +
constant-sky Fresnel shade → composite against the **opaque** scene depth.
Particles never touch the voxel mesher. Files:

- `ue-project/Shaders/VoxelFluidRender.usf` — the five kernels + the mirrored
  material law (every shading constant cites its line in
  `Tools/create_water_voxel_material.py`).
- `Source/VoxelEarthShaders/Public/VoxelFluidRender.h` + `Private/VoxelFluidRender.cpp`
  — the scene-view extension, pass builder, GPU timing.
- `VoxelFluidSim.h/.cpp` — one surgical addition: `RenderSlotBound`
  (render-thread published splat width; the renderer reads `Particles` + this
  and nothing else of the sim state).
- `VoxelFluidSubsystem.cpp` — cvars, per-tick settings marshal (fluid origin,
  sun from `UVoxelSkySubsystem`, radii), teardown ordering, `renderMs` in the
  perf line.

### The compositing hook, and the evidence for it

**Chosen: `FSceneViewExtensionBase::PrePostProcessPass_RenderThread`** (via
`FWorldSceneViewExtension`, registered lazily by the subsystem) — the
project's first post-opaque render hook.

- It is the engine's sanctioned "add RDG passes against scene textures" seam;
  UE 5.8 engine plugins ship on this exact override (verified in this box's
  tree: `ColorCorrectRegions`, `CompositeCore`, `PostProcessMaterialChainGraph`).
- It runs after all scene rendering, before post processing — the fluid
  tone-maps/blooms with the scene like the shipped water material does, and
  the opaque depth is final.
- One deviation from the naive recipe, found the hard way: the hook's
  `FPostProcessingInputs` argument is **Renderer/Internal** and does not
  compile from a project-scope module (UBT exposes `Internal/` includes only
  to engine-scope modules; verified in `UEBuildModule.cs`, and verified
  empirically — the include fails on its own sibling headers). The pass
  therefore leaves `Inputs` unused and gets the same uniform buffer through
  the **public** accessor built for exactly this situation:
  `UE::FXRenderingUtils::GetOrCreateSceneTextureUniformBuffer`
  (`FXRenderingUtils.h`, RENDERER_API), plus `GetRawViewRectUnsafe` for the
  view rect. No Build.cs change, no engine-module pretence.
- The rejected alternative — a translucent screen-quad material fed by a
  material parameter collection — cannot bind the particle StructuredBuffer
  at all (MPCs carry scalars/vectors), and re-enters the sort-key machinery
  whose hazards the water material documents.

Ordering: the subsystem enqueues the solver tick during the game tick, scene
rendering is enqueued after it, render commands execute in order — so the
splat always sees this frame's finalized particle positions.

### Pass architecture and buffers (numbers at 2560×1440, half-res 1280×720)

| # | pass (ProfileGPU name) | what | target, format, size |
|---|---|---|---|
| 1 | `VoxelFluidRender.Splat(n slots)` | instanced camera-facing quads, vertex-pulled by `SV_InstanceID` from the contract buffer (no vertex factory, no vertex buffer); PS computes sphere-impostor front depth; particles fully behind the opaque depth are killed here so they can neither pull the smooth through a wall nor tint water in front of one | MRT0 depth `R32_FLOAT` 1280×720 **3.69 MB** (BlendOp **MIN** = nearest surface, order-free); MRT1 thickness `R16F` 1280×720 **1.84 MB** (additive, order-free) |
| 2 | `VoxelFluidRender.SmoothX` | separable bilateral (depth-aware Gaussian), X | `R32_FLOAT` UAV 1280×720, 3.69 MB |
| 3 | `VoxelFluidRender.SmoothY` | same, Y | `R32_FLOAT` UAV 1280×720, 3.69 MB |
| 4 | `VoxelFluidRender.ShadeComposite` | full-res fullscreen triangle: depth-aware upsample (4-tap, silhouette-rejecting), smallest-difference finite-difference normals, Beer–Lambert by thickness, constant-sky Fresnel + glint, `SrcAlpha/InvSrcAlpha` into scene colour, `discard` wherever fluid depth >= scene depth | writes SceneColor (no new target) |

Total transient working set ~= **12.9 MB**, all RDG-pooled. Raster splats
over compute scatter was argued, not assumed: a scatter's per-thread
footprint loop is unbounded in the near field (a close particle covers
thousands of half-res pixels), while MIN/ADD blending is the rasteriser's
native atomic — and both blends are commutative, which is what makes an
unsorted particle draw correct. StructuredBuffer SRVs in a VS are SM5-legal,
so the renderer stays inside the solver's own SM5 gate.

Splat-vs-scatter, half resolution, and the depth-kill margin (one particle
radius, so half-buried bed particles still contribute) are all v0 decisions
the owner's screenshots can overturn cheaply — each is one pass or one
constant.

### Same-water guarantees (and the one stated approximation)

- Beer–Lambert mirrors the shipped material constant-for-constant: 160 UU
  folding depth, shallow (0.035, 0.26, 0.68) → deep (0.008, 0.055, 0.24),
  opacity 0.42 → 0.98, Fresnel F0 0.02 exp 5, sky tint (0.30, 0.46, 0.72)
  gated by `saturate(sunDir.z)`, glint tint/power (2.6, 2.45, 2.15)/900. Each
  cites its source line in the .usf.
- **Thickness is volume-normalised**: the impostor sphere at the default
  15 UU radius has ~14x the volume of the 10 cm voxel of water a particle IS
  (contract :27-29), so the accumulated chord is rescaled by
  `RestSpacing^3 / ((4/3)*pi*R^3)` — without this, particle water reads ~14x
  deeper/inkier than the datum water beside it. Radius changes stay
  volume-correct automatically.
- Sun comes from the same ephemeris that feeds the material's MPC
  (`UVoxelSkySubsystem`, toward-the-sun convention, `VoxelEphemeris.h:48`).
- **Stated approximation**: the material's tint is lit by the translucent
  lighting path; a post-opaque pass has no light grid, so v0 lights the tint
  with `0.18 + 0.82 * saturate(N.sunDir) * dayGate` — the only two invented
  constants in the pass, flagged in the .usf. No scene-colour read anywhere
  (ban 1 — the composite is a hardware blend); refraction is absent at v0
  (the sanctioned normal-perturbed depth trick is a later knob).
- Scene captures / reflection captures skip the pass (presentation layer;
  a capture re-running it would double the cost invisibly).

### Verification done here (no editor launched, per constraints)

- **UBT: `Result: Succeeded`** (VoxelEarthEditor Win64 Development,
  MSVC 14.51), zero warnings after the one deprecation fix
  (`GetProjectionMatrix` → `GetViewToClip`).
- **Offline dxc** (`tools/dxc`): all five entry points compile to **both
  DXIL and SPIR-V** — `-T vs_6_0/ps_6_0/cs_6_0 -E <entry> -O3
  -D SM5_PROFILE=1 -D COMPILER_HLSL=1 [-spirv -fspv-target-env=vulkan1.1]`,
  include root = a junction dir mapping `Engine -> UE_5.8/Engine/Shaders` and
  `VoxelEarth -> ue-project/Shaders` (the reusable offline pattern for any
  /VoxelEarth shader). 10/10 green. `tools/lint-shader-ub.py`: clean.
- Not verified here (needs the editor): pixels on screen, renderMs numbers.
  That is the measurement section below.

### Perf line change

`renderMs=<x.xx>|pending|off` now sits after `simGpuMs` in the 1 Hz line.
`off` = cvar 0; `pending` = armed but no pass has completed a GPU timing
(includes "nothing spawned yet"); a number = newest completed GPU time of the
whole `VoxelFluidRender` span, measured with the same `RQT_AbsoluteTime`
bracket as `simGpuMs`, so the two are directly comparable. The ran-flag rule
holds: a renderer that never ran can never print `0.00`.

### F. Renderer measurement runs (integrator)

All PIE/game, one editor per box; conditions + captures per standing rules,
no verdicts. The GPU budget context (plan, risk #1): frame p50 15.16 /
p95 20.94 ms at 1440p with the tail being GPU — **the number that matters is
the p95 delta with the renderer on**, not the p50.

**F1. Cost at the spike population (the gate number)**
```
voxel.Fluid.Enable 1
; wait for "initial fill COMPLETE" in the log, then:
voxel.Fluid.Render 1
voxel.Fluid.Spawn 5000        ; then re-run at 100000 for the spike population
```
Read: `renderMs` at 5 k and 100 k alive; `ProfileGPU` once at each population
for the per-pass split (`VoxelFluidRender.Splat/SmoothX/SmoothY/
ShadeComposite`); `stat unitgraph` p95 with `voxel.Fluid.Render 0` vs `1`
(everything else identical) — that A/B delta is the budget verdict.

**F2. Same-water A/B (the look gate)**
At a lake-survey site with datum water in frame (site verified first, per the
site-verification tools): one capture pair, `voxel.Fluid.Render 0/1`, same
pose, dam-break pool beside the lake. The particle pool and the lake should
read as the same water (tint, depth darkening, sky sheen). Conditions in the
caption; owner judges.

**F3. Composite correctness probes**
- Fluid behind a ridge: walk the camera so terrain occludes the pool —
  no water shows through (the splat depth-kill and the composite discard
  both fire).
- Grazing angle at the pool: Fresnel brightens toward the horizon and the
  surface goes more opaque (the material's own opacity fold, mirrored).
- `voxel.Fluid.Render.RadiusUU 10` vs `25`: surface tightness changes,
  water COLOUR does not (thickness normalisation holds).
- `voxel.Fluid.Render.SmoothRadiusPx 1` vs `12`: marbles vs sheet — pick by
  capture, not by taste.

**F4. Cvar surface for these runs**
```
voxel.Fluid.Render                0|1   (default 0)
voxel.Fluid.Render.RadiusUU       15    (sprite radius, UU)
voxel.Fluid.Render.SmoothRadiusPx 6     (bilateral radius, half-res px, 1..32)
```

---

## Addendum: toroidal occupancy addressing (contract item 4, 2026-08-09)

Appended by the toroidal build; everything above is the integrator's and the
renderer's and is unchanged apart from one superseded bullet in Deviations.

### The problem it removes

A recentre used to invalidate all 16 MiB. The volume was flat -- a bit's address
was its window coordinate -- so moving the origin renamed every bit and the only
correct response was to throw the lot away. Refilling is 262,144 bricks against
a mesher with ~893 bricks/s of spare capacity: multi-second, with the unfilled
part solid and the water in it frozen. That cost is why v0 latched the origin
once and never moved it, which in turn is why the fluid could not follow a
camera that walked.

### What changed

The buffer is now a **rolling window**. A bit's storage slot is

```
storage = (window coordinate + FluidVolumeWrapOffsetVoxel) mod 512
```

where the wrap offset is how far the window has slid since it was created. The
bits still in view therefore keep their slots across a move: only the newly
exposed cells are invalidated, and the cells that left are neither cleared nor
copied -- the entering ones land on exactly their slots.

Three coordinate spaces, and they are worth keeping apart in your head:

| space | what it is | who uses it |
|---|---|---|
| WORLD | planet voxel coordinates | `VoxelFluidSolidAtVoxel`'s argument; brick packing |
| LOCAL | world minus the origin, 0..511 | **everything geometric**: particle positions (UU), the collision walk's boundary planes, the projection, the boundary/basin boxes, the region/refill boxes |
| STORAGE | local plus wrap offset, mod 512 | one expression in `VoxelFluidCollision.ush`, one in `VoxelFluidOccupancy.usf`, and `vxc::fluidVolumeWordIndexLocal` |

**The window bounds test did not move and must not.** It runs on LOCAL, and it
is what keeps "outside the volume is solid" true. Dropping it because "the
modulo wraps harmlessly" makes a voxel one past the west face read the terrain
51.2 m east -- the failure that reads as worldgen.

**Why the offset is accumulated motion, not `world mod 512`.** Both are rolling
windows. The simpler one additionally requires the origin's x to be a multiple
of 32, or a 32-voxel output word straddles the seam and the builder's whole-word
write becomes a read-modify-write that two regions can race on. The origin is
the camera's voxel minus 256. Accumulated motion moves the alignment
requirement onto a quantity the host controls: every step is a multiple of
`vxc::kFluidRecentreStepVoxels` (64), so the seam is always word-aligned
whatever the initial origin was. Cost: one `uint3` uniform.

### The cost, in the units the budget is in

A 64-voxel (6.4 m) step exposes 8x8 = **64 cells of the 512** the initial fill
queues -- 1/8 of the volume, ~2 MiB of bits. At the default
`voxel.Fluid.Occupancy.RegionsPerTick 8` that is 8 ticks, ~0.13 s at 60 Hz.
(The volume's old cost note guessed "~1/50 for a one-chunk step". That was
optimistic about the step size, not about the mechanism: 1/8 at this quantum,
1/16 at the 32-voxel floor. Either way it is a bounded slab instead of 16 MiB.)

A camera outrunning the refill -- faster than 6.4 m per 8 ticks, i.e. ~48 m/s --
grows the queue rather than breaking anything: unbuilt is solid, so water at the
leading edge freezes and nothing leaks, and the backlog shows in the perf line's
`occupancy=<built>/<deferred>`. **Flying is the case to measure.**

### The call the subsystem should make (NOT WIRED YET)

Nothing calls `RecentreTo`. `UVoxelFluidSubsystem::LatchOrigin` still calls
`SetOriginVoxel` once and the origin never moves, exactly as before. The
machinery, the tests and the contract are in place; the policy is one function
in the subsystem, and it belongs to whoever owns that file:

```
// Once per game tick, after the view origin is known, BEFORE ProcessOccupancyQueue.
CamVoxel   = WorldToVoxel(ViewOriginUU)
WantOrigin = CamVoxel - DimVoxels/2                      // re-centre exactly
Drift      = WantOrigin - OriginVoxel                    // per axis
if (max|Drift| < 96) return;                             // hysteresis: 1.5 steps
Step       = round(Drift / 64) * 64                      // whole recentre steps
Occupancy->RecentreTo(OriginVoxel + Step, RefillCells, Delta, Error)
```

then, in the same tick:

1. **Update the subsystem's own origin state** from the new origin --
   `OriginVoxel`, `FluidOriginWorld` (= origin x 10, contract item 1), the
   boundary-box centre, the basin box, and `SetFluidSpillIntercept`'s XY box.
   These are all derived per tick already; they must be derived from the NEW
   origin or the water is offset from the terrain by the step.
2. **Queue the returned cells** exactly the way the initial fill queues its 512:
   `PackRegionBricks` against the new origin, `UpdateRegion`, ordered centre-out
   so terrain near the camera lands first. They are already snapped; no
   arithmetic needed. Ignoring them leaves the entering slab solid *forever*.
3. **Rebase the particles.** `TakePendingRebaseDeltaVoxels()` on the game thread,
   then `FVoxelFluidOccupancyVolume::AddRebaseParticlesPass(GraphBuilder,
   ParticlesUAV, SimSlotBound, Delta)` in the solver's graph, **between solver
   ticks** -- first thing after the occupancy `AddPasses` the ordering guard
   already pins. Skipping it slides the water 6.4 m sideways relative to the
   terrain, once per recentre, cumulatively. (Contract item 8.)

**The two numbers.** Step 64 voxels because that is both the addressing quantum
and the fill-cell size. Trigger 1.5 steps (96 voxels, 9.6 m) so that
round-to-nearest leaves the drift inside +/-32 and the camera must travel a
further 64 voxels to trigger again. Without the 1.5 a camera loitering on a
boundary pays 64 cells of refill every few frames.

`SetOriginVoxel` is unchanged and still correct for the two cases where
preserving nothing is right: the first latch, and a teleport.

### Verify gate

`voxel.Fluid.Occupancy.Verify` now applies the same wrap on both sides
(`fluidVolumeWordIndexLocal`). It had to: the cursor names a cell in the WINDOW,
and comparing at a flat index would compare two different voxels the moment the
offset is non-zero and report every one of them as a FAIL. This is the only
change made to `VoxelFluidSubsystem.cpp` by this work -- two lines.

### Verification done here (no editor launched, per constraints)

- **UBT `Result: Succeeded`** (VoxelEarthEditor Win64 Development, MSVC 14.51),
  no new warnings.
- **`vxc_tests` 572 PASS / 0 FAIL**, including 11 new cases: wrap indexing as a
  bijection across the seam; whole-word preservation (and the unaligned offset
  that would break it); verify-gate equivalence for a region straddling the
  seam; outside-the-window-is-solid against the exact slot that would alias;
  the collision walk crossing the seam in both directions plus free motion
  across it; entering-cell enumeration for straight/diagonal/backwards/beyond
  moves with a no-double-count assertion; delta and offset alignment; a full
  end-to-end recentre proving the far side is byte-identical, that the entering
  slab really would serve the old terrain, and that marking closes it; two full
  laps of the ring rebuilt only incrementally; and the particle-rebase precision
  bound.
- **Offline dxc**: all 14 fluid entry points to both DXIL and SPIR-V, 28/28
  green (occupancy x3 including the two new kernels, sim x11 with
  `VOXEL_FLUID_HAS_COLLISION=1`). `tools/lint-shader-ub.py` clean on the three
  files this work owns.
- **Not verified here** (needs the editor): that a recentre looks right, and the
  refill latency at speed. ~~Nothing calls `RecentreTo` yet, so there is nothing
  in-editor to look at until the subsystem policy above is wired.~~ **The policy
  was wired 2026-08-10** — see the last addendum in this file.

---

## Addendum: owner playtest fixes — faucet blast, stuck orbs, faucet beacons (2026-08-09)

Appended by the playtest-fix pass; everything above is unchanged. Owner
feedback on the PBF water: (1) faucets "blast water orbs up and out
explosively"; (2) landed particles "just sit on mountainsides" instead of
flowing downhill and pooling; (3) wants water to stream out per tick at the
flow rate, never as bulk dumps; plus a request for visible faucet markers.

### Root causes found (file:line at this commit)

1. **The blast is over-density at the source, twice over.**
   - A SPRING emitted with ZERO velocity at one point 30 UU above the ground
     (`VoxelFluidSubsystem.cpp`, RefreshHeadwaterFaucets' spring arm: `F.Velocity`
     never set), and the spawn kernel jittered in a 30 UU XY disc around that
     one point (`VoxelFluidSim.usf`, faucet arm of `FluidSpawnMain`). Every
     tick's particles landed inside the standing pool at the emit point --
     inside each other's 25 UU kernel radius -- and the density constraint
     resolved the overlap the only way it can: by ejecting the clump. That IS
     the orb fountain.
   - The schedule allowed bursts: `FluidFaucetAccumulator::addMicros` pays the
     WHOLE elapsed backlog out in one call (correct -- it is drift-free by
     design), and the only per-tick bound between it and the spawn kernel was
     the shared `voxel.Fluid.MaxSpawnPerTick` budget (4096). A 2 s hitch at
     253/s put a 507-particle dump on one point in one tick. Sill faucets were
     worse: `Emit = min(owedParticles, Budget)` -- an entire spill event's
     backlog in one tick, bounded only by the budget.
   - Verified while here: the deferral path (unbuilt occupancy) does NOT
     accrue a backlog -- it skips `addMicros` entirely, so no dump on
     fill-completion. The dump risk was hitches and clamp surpluses, both now
     capped.

2. **Stuck orbs: two findings, one fix and one statement.**
   - The finalize contact response is structurally correct -- tangential
     velocity DOES survive contact (`FluidFinalizeMain` lifts only the normal
     component of `pOld`), so the "resolve leaves v=0" hypothesis is FALSE for
     single-face contact. But the lift used one dot-product projection against
     an ACCUMULATED normal that is non-unit at corners (`(1,0,1)` from a
     two-axis resolve), which under-corrects and leaves a residual approach
     speed of `(|n|^2 - 1) * dot(d, n)`. Replaced with the exact per-axis
     form (pOld's clamped-axis components set equal to pNew's) -- identical
     for one face, exact at corners, tangential untouched. Contract item 7
     amended in place; `VoxelFluidCollision.ush`'s usage comment updated.
   - The rest is REAL PHYSICS, stated rather than faked: a voxel slope is a
     staircase of flat 10 cm treads; an isolated particle at rest on a tread
     has zero tangential force (gravity is normal to the tread) and no PBF
     neighbours to push it, so a lone droplet sticks -- as a real droplet
     does. The cure is emission coherence (fix 1): particles emitted as a
     moving line arrive as a STREAM whose density gradient pushes the front
     forward over the step edges, exactly like real water on stairs.
   - XSPH viscosity was considered and stays deferred: it needs a
     neighbour gather over FINAL velocities -- one more full sorted-domain
     pass PLUS pulling velocities through `CellEntries -> ParticlesRW`
     slot-indexed reads, which is precisely the random-access traffic the
     perf pass spent its effort removing (the 20-28 ms at 100k lived there).
     Not paid for a smoothing nicety while emission coherence is the actual
     fix; revisit only if the owner's captures still read "grainy" after
     this pass.

### What changed

- **Springs and sills emit MOVING DOWNHILL** (`kSpringSeepSpeedUU` 150 UU/s
  along the terrain gradient, 50 UU/s downward bias). The direction is
  central-finite-differenced from `GetSurfaceHeightUU` (+/-100 UU) -- the
  same surface authority the faucet Z uses, never rebuilt elsewhere -- on the
  game thread, cached per faucet at gather refresh (springs, 4 calls per
  spring per 10 s) or event arrival (sills, once). Flat ground (< 0.5 %
  slope) keeps zero velocity: a plateau spring pools in place, correctly.
  Edge inflows already carried the channel direction; unchanged.
- **Emission is a line, not a point**: faucet-mode spawns jitter along a
  +/-30 UU LINE perpendicular to the stream velocity (new `SpawnJitterDirUU`
  uniform / `FVoxelFluidSpawnRequest::JitterDirUU`, host-computed as the
  horizontal perpendicular; zero falls back to the old disc for still
  sources), plus a +/-1 UU isotropic break so the line's constraint gradients
  cannot cancel in pairs.
- **Source anti-burst, per faucet** (the owner's ask #3): per-tick emission
  <= ceil(2 x rate x dt) and a rolling 1 s window <= ceil(rate), both derived
  from the faucet's OWN Q. What the clamps hold back is carried -- up to ~1 s
  of rate via the new `FluidFaucetAccumulator::carryBackParticles` (exact
  inverse of the payout; `test_fluidlifecycle.cpp fluid_faucet_carry_back`)
  -- and only the remainder routes through the scalar graph, exactly as the
  backpressure surplus always has. Sill faucets drain at
  `kSillDrainPerSecond` (253/s, the DefaultQ worked example) instead of
  budget-sized dumps; their surplus stays owed on the entry as before. The
  ledgers still close: nothing new is dropped anywhere.
- **Faucet beacons**: `voxel.Fluid.DebugFaucets` (default **1**) draws a
  sphere + 20 m vertical line at every active faucet each tick -- MAGENTA
  for springs, sill outlets and the camera faucet; ORANGE for edge inflows.

### Verification done here (no editor launched, per constraints)

- UBT `Result: Succeeded` (VoxelEarthEditor Win64 Development).
- Offline dxc: all 11 `VoxelFluidSim.usf` entry points to both DXIL and
  SPIR-V with `VOXEL_FLUID_HAS_COLLISION=1`, 22/22 green.
  `lint-shader-ub.py`: `VoxelFluidCollision.ush` / `VoxelFluidContract.ush`
  clean; `VoxelFluidSim.usf` is a float-typed solver file the integer-kernel
  lint has never been clean on (20 findings at the previous commit, all
  scientific-literal/float false positives; this pass adds 2 more of that
  same class and no new write/shift/division findings).
- `vxc_tests` 585 PASS / 0 FAIL, incl. the new `fluid_faucet_carry_back` case
  (carry-back then re-drain equals never having taken the particles out).
- NOT verified here (needs the editor): the look. Next run, expect: streams
  leaving springs sideways-downhill as a ribbon instead of a fountain;
  spill saddles trickling continuously; magenta/orange beacons at every
  source (turn off with `voxel.Fluid.DebugFaucets 0` for captures);
  mid-slope stream fronts advancing over voxel steps where lone droplets
  still legitimately stick. Captures at the pinned poses, conditions
  stated, owner judges.

---

## Addendum: the recentring policy is wired (2026-08-10)

Appended by the recentring pass. Everything above is unchanged apart from the
superseded "No recentring (v0)" bullet. This closes the last gap between the
toroidal machinery (previous addendum, "NOT WIRED YET") and the water: **flying
past the 51.2 m window no longer freezes the fluid, and toggling
`voxel.Fluid.Enable` is no longer the workaround.**

### The policy, and where it runs

`UVoxelFluidSubsystem::MaybeRecentre`, once per game tick, after the view origin
is known and **before** `ProcessOccupancyQueue` â€” that ordering is the whole
policy's one constraint, because the queue packs against `OriginVoxel` and the
faucet/sink refreshes and the spawn assembly all derive from `FluidOriginWorld`
later in the same tick.

```
WantOrigin = camera voxel - 256 in XY, GroundVoxelZAtCamera() - 128 in Z
Drift      = WantOrigin - OriginVoxel                 (int64, per axis)
if (max|Drift| < 96) return                           (1.5 steps of hysteresis)
Step       = round(Drift / 64) * 64                   (halves AWAY from zero)
RecentreTo(OriginVoxel + Step, ...)
```

The two numbers are the header's (`VoxelFluidRecentre::StepVoxels` 64,
`TriggerVoxels` 96, both `static_assert`ed against
`vxc::kFluidRecentreStepVoxels`). Z re-anchors to the **ground column**, not the
camera, by the same rule the first latch uses â€” one definition,
`DesiredOriginVoxel`, shared by both, so the window keeps tracking terrain as
the camera crosses a valley instead of only at the latch.

A move of a whole window (>= 512 voxels in one tick, ~3 km/s) is a **teleport**:
it routes to `LatchOrigin`/`SetOriginVoxel` instead of a slide, because nothing
in view survives it and a slide would pay a particle rebase to move water that
shares no terrain with where it is going.

### The particle half (contract item 8)

`TakePendingRebaseDeltaVoxels()` is taken on the game thread **at mailbox-post
time**, not inside the recentre: a tick that does not post must leave the
obligation accumulating on the volume rather than dropping it. It rides
`FVoxelFluidRenderState::PendingRebaseDeltaVoxels` (accumulated, since an
unconsumed post is overwritten every tick) and
`FVoxelFluidRenderExtension::PreRenderViewFamily_RenderThread` dispatches
`AddRebaseParticlesPass` **before** `AddSimPasses` â€” i.e. between solver ticks,
in the renderer's own graph, the same seam the sim rides.

**The stale-origin gate, which is new and load-bearing.** The render thread is a
frame behind, so it can consume args built against an origin the game thread has
already moved: un-rebased particles solved against a volume bound one step away
is up to 12.8 m of displacement, and the density constraint resolves that by
ejecting the water out of the rock it now sits in (the blast artifact, once per
recentre). The args now carry the origin they were built against
(`PendingSimArgsOriginVoxel`); a mismatch drops that tick, counted, and leaves
the delta owed. The splat likewise draws against the **particle buffer's own**
origin (`ParticleOriginWorld`), not the settings', so a dropped frame keeps
drawing the water where the water is.

### The origin-relative audit (the table this pass was really about)

| quantity | where | how it stays correct |
|---|---|---|
| `FluidOriginWorld` | subsystem | recomputed from the new origin, same tick |
| queued fill regions | `FVoxelFluidLifecycle::PendingRegions` | **volume-LOCAL**; translated by âˆ’delta, non-fitting dropped (their space is an entering cell) |
| entering cells | `RecentreTo` out | requeued centre-out; ignoring them freezes the leading edge *forever* |
| spill intercept box | `SetFluidSpillIntercept` | re-armed over the new XY footprint (`OnOriginMoved`) |
| sill faucets | lifecycle | outlets the window left are **refunded** to the ledger, not left deferring |
| headwater/crossing gather | 10 s cadence | forced to refresh on the move |
| basin sink box + datum | 1 s cadence | invalidated + forced to refresh |
| particle positions | GPU buffer | the rebase pass, above |
| spawn `CenterLocalUU` | tick args | already `world âˆ’ FluidOriginWorld` per tick, after the recentre |
| basin box/datum local | tick args | same, and the world-space source is refreshed |
| boundary inject voxel | reconcile | `OriginVoxel + 256` per use |
| `NotifyTerrainDirty` | listener | snaps against `OriginVoxel` at call time |
| renderer origin | render settings | marshalled per tick; the splat uses the buffer's origin |
| `BoundaryCenterLocalUU`, half extent, `GroundZLocalUU`, clamp margins | tick args | origin-relative **constants** â€” they describe the cube, not the world, so a move cannot invalidate them |
| verify gate | subsystem | `EditEpoch` bumped on recentre â†’ `stale`, never a false FAIL |
| camera faucet (`voxel.Fluid.Emit`) | subsystem | **stated, unchanged**: keeps its latched world point; re-issue the command to move it |
| debug draw | subsystem | **stated**: one readback's worth of points can plot against the new origin for a frame after a move |

### Counters (perf line, ran-flag rules)

```
recentre=<n>(cells=<n>,rebase=<passes>/<slots> slots,miss=<n>,stale=<n>,
             tele=<n>,refused=<n>,checks=<n>)   |   off
```

`off` means no origin is latched. `checks=` is the ran-flag that separates "the
camera has not moved 9.6 m" from "nobody is calling the policy" â€” which is
exactly what v0 looked like, and the two are identical from the water's side.
`rebase=` must track `recentre=`: a recentre with no rebase behind it is water
sliding 6.4 m away from the terrain, so `miss=` is on the same field rather than
hidden. `miss=` is only counted when particles actually existed.

### Verification done here (no editor launched â€” the owner is playing)

- **UBT compile-verify, `Result: Succeeded`** for both touched translation units
  (`-singlefile`, which is also what Live Coding limits a build to while a
  session is active): `VoxelFluidSubsystem.cpp` and `VoxelFluidRender.cpp`,
  VoxelEarthEditor Win64 Development, MSVC 14.51. No new warnings. One latent
  include fix on the way: `VoxelFluidRender.cpp` was reaching
  `TStaticBlendState` et al. through the unity blob and does not compile on its
  own without `RHIStaticStates.h` (any modified file takes the adaptive
  non-unity path, so this bites whoever edits it next, not this change).
- **`VoxelEarth.Fluid.RecentreTrigger`** added (bottom of `VoxelFluidSubsystem.cpp`):
  trigger/no-trigger either side of 96 on every axis at a planet-scale origin,
  mirror symmetry of the rounding, whole-step alignment, residual <= half a step,
  one-move convergence (no chatter) swept over 96..8192 in both directions, the
  Z-only trigger the ground anchor needs, and the teleport hand-off. **NOT RUN**
  â€” it needs an editor process and the owner is in one; run it with
  `-ExecCmds="Automation RunTests VoxelEarth.Fluid; Quit"` at the next
  opportunity. What *was* run: the shipped bodies of `SnapDriftToStep` /
  `ComputeRecentreOrigin`, extracted textually and compiled standalone against a
  shim (clang++ 20, `-std=c++20`), pass every assertion the test makes.
- **Not verified here** (needs the editor): that flying keeps the water alive,
  the refill latency at speed, and that no blast appears at a recentre. Read
  `recentre=` against `occupancy=<built>/<deferred>` while flying; captures at
  pinned poses, conditions stated, owner judges.
