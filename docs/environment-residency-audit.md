# Environment residency and LOD audit

Audit: September 9, 2026. Updated September 10 through build 59, profiled hill
route 6, experimental LOD capture 5 and matched cache walks 18/19.

Rendering visibility, resident placement data, source voxel banks and reusable
mesh memory are separate costs. A resident voxel/page count is not a count of
visible tree voxels. Tree pages also contain terrain.

## Trees

Trees participate in voxel terrain composition and distance cascades. Current
walk configuration uses 100 mm through 64 m, then doubles voxel pitch at each
successive distance band. They are not individually rendered HISM objects.
Radial residency includes behind-camera data; screen traversal and opaque depth
termination limit rendering work. Frustum-only residency would need turn-rate
and reveal testing before it could safely replace this behavior.

Conservative request bounds pruning is already implemented. Walk 17 pruned
429,355 of 494,089 candidate request references; these are repeated references,
not distinct trees. Do not interpret the reduction as equivalent GPU savings.

Source recheck: `VoxelWorldSubsystem.h:190` defines the distance presets;
`VoxelMarch.usf:1460` bounds work to view pixels and `:1504-1558` limits rays by
populated opaque prepass depth. `VoxelWorklistAssetStamp.usf:116` samples coarse
source columns rather than rendering every original voxel. Request pruning in
`VoxelWorldSubsystem.cpp:24251` happens before span copying, but earlier
footprint resolution and appearance preparation can still incur CPU cost.
Current page statistics do not separate visible trees from resident terrain;
they cannot quantify the cost of invisible trees alone.

## Understory

`VoxelDetailAssetSubsystem.cpp` currently configures:

- 256 m default placement ring; instance distance cull at that outer radius.
- Placement groups released beyond 1.15 times the radius (294.4 m by default).
- HISM instancing with no collision. Meshes are shared by species/seed key.
- One default geometry LOD; reduced LODs require `-VoxelDetailMeshLOD`.
- Meshes, HISM components and geometry-known keys retained after groups unload.
- Geometry-derived XY wind bounds across all LODs; no Z padding for the current
  shader. Missing geometry falls back to 30 cm; nonfinite input is rejected.

The configured cull start is 85% of the radius, but the material does not use
PerInstanceFadeAmount. This is not evidence of a gradual fade.

Recent performance walks used a 48 m ring, not the 256 m default. At equal
density, the latter covers about 28.4 times the area; this is an area comparison,
not a measured instance-count or frame-cost multiplier.

Real-asset LOD capture 2 verified appearance and approximately 24–28% fewer
triangles, at about 1.70–1.76 times the mesh resource size with an added LOD.
Automatic HISM capture 3 passed reed/shrub transitions and the shrub 48 m cull
positive control, but failed grass/daisy transition predictions. UE's actual
cluster bounds explain the conservative switch distance. A corrected harness
was subsequently compiled in build 51. Real-asset capture 4 passed with warnings:
all four automatic switches passed, as did the shrub cull positive control.
Grass switched from 517 LOD0 pixels at 5.748 m to 428 LOD1 pixels at 6.266 m.
Shrub visibility was 131 pixels inside its cull range, zero outside, and 96 at
the same outside pose with culling disabled. This validates the four tested
cases, not all species, default-radius gameplay performance or every camera pose.
Warnings concern CPU physics data on noncolliding transient test meshes.

Build 53 capture 5 also passed all four automatic transitions and the cull
positive control with the tighter wind bounds. The grass cluster half-diagonal
fell from 82.29 cm to 33.02 cm; its predicted actual switch moved from about
6.01 m to 5.51 m. This improves overly conservative bounds, but does not prove a
whole-scene GPU gain. Reduced geometry is still opt-in, including in the current
route 3 run. The 25 mm source voxels become shared triangle meshes: they are not
independently ray marched per understory instance in this configuration.

The default still gives every plant the same end-cull distance. Build 59 adds
an opt-in `-VoxelDetailSizeCull` policy using wind-expanded mesh dimensions and
the current unit-scale transform contract. It proposes a 32m floor clipped to
the configured ring, scales distance with model size, and never exceeds that
ring. Malformed bounds fall back to the full ring. Cached and transient meshes
share this installation path. Focused helper tests pass; actual gameplay
distance boundaries, popping, camera turns and whole-scene costs remain untested.

## Source and mesh memory

`AssetBankLibrary::bankFor` parses every accepted VXA in a species directory on
first access and retains the bank until reconfiguration. This preserves stable
seed indexing but can retain variants not currently placed. Loading only chosen
variants must preserve validation and deterministic indexing, not renumber seeds
according to cache residency.

Understory group unloading does not evict reusable meshes. Safe eviction needs
to account for pending instances, in-flight resolver snapshots, geometry-known
state and UObject references together. Removing just the mesh map entry is unsafe.

The September 10 audit found geometry/cache requests processed before stale-group
distance rejection. Build 59 now rejects normal departed-group results before
admitting those resources. Geometry-only fallbacks still fulfill existing
promises. Focused guard tests pass; rapid departure/revisit runtime tests remain
pending. This change does not evict existing meshes or cancel earlier loads.
Unapplied instances
live in worker results and then `Groups[].Instances`; `KeyGroups` can reference
an instance awaiting its mesh. Cache validation also requests all mesh LODs
resident for 30 seconds. None of these allocations is prevented by render culling.

`GeometryKnown` is a promise to in-flight worker snapshots. Clearing it and
evicting a mesh while a worker holds an older snapshot can produce a live group
with no geometry delivery. Resource aliases also matter: multiple MeshKeys can
share one cached UStaticMesh, and BuiltMeshes can contain repeated roots.

A proposed opt-in retirement pilot would pause new detail resolve dispatch,
drain outstanding work without blocking the game thread, then retire a bounded
number of unused keys. Groups, pending geometry, cached install requests,
package loads/validation and fallbacks must pin resources. Destroy components
through normal UE lifetime handling, remove every obsolete root only after the
last shared user is gone, and avoid synchronous GC/render flushes. This is not
implemented; released references would not prove immediate VRAM release. An
active/pending working set larger than the budget must be reported explicitly.

## Remaining performance acceptance

1. Finish platform cook and shipping runtime cache validation. All 339 private
   schema-2 meshes now pass fresh-process attribute/bounds/material validation
   without regenerated mesh builds. Explicit asynchronous editor cache use in
   walk 19 removed runtime mesh construction (walk 18 moving build maximum
   160.0 ms versus approximately zero in walk 19). Detail tick still reached
   33.8 ms; sprint frame tails worsened in this single matched pair. This is not
   overall performance acceptance or automatic production cache enablement.
2. Validate actual automatic LOD switches, then compare matched gameplay runs
   with and without LOD. Triangle savings alone do not establish a frame gain.
3. Design size-aware detail visibility ranges using projected size and wind-safe
   bounds. Preserve ecological placement determinism; change presentation range,
   not the canopy/neighbor dependency footprint.
4. Bound unused mesh residency and measure cold loads, revisits and long travel.
5. Validate the intended default density/radius, camera turns and visible popping.
   Retain readable ground cover, thickets and natural openings.

Private fixture meshes and bake packages are not endorsed library content.

## September 10 performance follow-up

The completed build-57 predictive-resolve OFF/ON pair passed all eight walking
checks in both arms, with 4,442 initial detail instances, matching input pins
and the same first March dispatch shader identity. Both use a 48 m ring,
experimental detail LODs and the editor mesh cache. Baseline module hashes were
recorded after its run, not retroactively asserted as start-time pins.

Walking frame median changed 25.10 to 24.94 ms; p95 changed 168.99 to 93.70 ms.
Sprint median worsened 28.31 to 37.63 ms, while p95 improved 290.84 to 222.68 ms.
The predictor's largest reported tick was 0.855 ms. These mixed results leave
prewarming opt-in. This experiment changes CPU resolve scheduling; it does not
demonstrate better culling, reduced mesh residency or default-range performance.
Evidence: `asset-forge/out/ecological-placement/walk-capture-23/predictive-comparison.json`.

Priority for the visibility/residency work is to measure and bound unnecessary
detail work before promoting defaults: size-aware presentation distances,
matched automatic-LOD gameplay, safe rejection of departed-group resource work,
and bounded unused mesh retention with turn/revisit testing. Source-bank lazy
loading is a separate change and must preserve authoritative variant indexing.
Current instrumentation cannot honestly report a distinct count or memory size
for invisible tree voxels; terrain page totals must not be used as that number.

Walk 23 does directly report 487 loaded source files and 104,021,866 bytes
(about 99.2 MiB) of source-bank residency. That is the entire private fixture's
variant file count, despite only a local region being presented. This counts
the bank's reported grid footprint, not total CPU memory or GPU mesh/page
memory. It substantiates eager source retention, but does not identify which
fraction of those bytes belongs to currently invisible instances.

## September 10 culling validation follow-up

The size-based policy changes drawing distance, not the placement/resolver ring.
It therefore cannot by itself prevent all distant placement work, source-bank
loading or shared mesh retention. A 32m grass cutoff inside a 256m placement ring
must not be reported as eliminating those plants from CPU or GPU residency.

The current real-asset LOD harness builds transient meshes and uses a fixed48m
bramble visibility control. Passing that harness with DetailSizeCull enabled
would not exercise the production policy installation path or cached meshes.
An explicit cached/transient size-policy boundary harness is compiled in build64;
no such runtime acceptance is claimed yet.

Separately, resource validation was repeatedly counting solid voxels by scanning
source material runs. The count is now derived once during AssetGrid parsing.
All12 native AssetGrid tests pass, including real assets, copy/move and failure
semantics. UE TerrainPagePreparation also passes in coherent build64; runtime speedup remains unverified. This is a
CPU validation optimization, not a culling or memory reduction.

Confirmed source mechanism in UE StaticMesh.cpp: generated sourceLODs inherit automatic PercentTriangles=.5^L and BaseLODModel0 until reset. Persistent adapter must preserve authored meshes explicitly. Full256m comparisons must wait for corrected cache; old cachedLOD benchmark is not equivalent to authored geometry.

## Corrected authored-LOD cache and actual culling validation

Build67 now preserves authored source LODs rather than accepting the engine's
automatic reduction defaults. The real four-source persistent-builder regression
passes. The new immutable `temperate-authored-lods-full-1` private cache has339
models; all339 unique meshes have matching authored/built triangle-count proofs,
and all339 passed fresh-process schema2 verification. Its manifest SHA256 is
`4fc65fadb332430459270fc77c8710ff80f522963d663f574ac3f45a0f5b8b36`.
Geometry/appearance source hashes, LOD counts and LOD0 triangle counts are unchanged.
The corrected distant LODs differ in323 models. Earlier cache-on performance
measurements describe the older reduced geometry and cannot establish performance
for this corrected cache.

Actual size-policy capture3 passed128 cached/transient comparisons with zero
warnings/failures: exact color and mask equality at tested near/far poses, two
perspective FOVs and wind off/on. All32 outside captures were empty; all96 positive
controls were visible. Distances: daisy32m, grass48m, reed160m, bramble208m, using
the256m configured ring. All128 actual-material checks passed without fallback;
start/end DLL/cache hashes matched. Evidence: detail-size-cull-real-tests-3 and
detail-size-cull-real-capture-3 under asset-forge/out/ecological-placement.
This is four-seed isolated rendering coverage, not all-species gameplay popping,
lighting, performance or residency acceptance.

Updated full-library presentation analysis is detail-presentation-analysis-2/report.json:
last-LOD median1496, p9045968, maximum189640 triangles. Its aggregate buffer and
circle-area estimates remain proxies, not measured VRAM or frame savings.
A new actual256m gameplay OFF/ON size-cull comparison now uses the corrected cache
with experimental LODs enabled in both arms. Full-scene results remain pending.

## Full256 comparison invalidated; validation now fails closed

Capture25 passed eight movement checks but its wrapper exited1 because UnrealEditor-VoxelEarth.dll changed during the run. Capture24/25 is NOT an accepted performance comparison. The descriptive comparison artifact is marked valid:false; no culling speedup is claimed. Another session's headless UE captures/builds were observed. Corrected339-mesh cache and isolated128 rendering comparisons remain separate accepted evidence.

The walking wrapper now persists run-validation.json, monitors competing UE/compiler processes and module metadata, and records end module/input/artifact hashes. The comparator requires a passing receipt bound to all files and three runtime modules (Earth, Shaders, UI). Missing historical receipts are not reconstructed. Four Python tests passed including changed-DLL, failed/running/missing receipt and artifact-tamper rejection; PowerShell syntax parse passed. Fresh OFF/ON captures are required after a coherent build on a clear machine.

R0 diagnostic patch is staged outside live source in .scratch/r0-entry-profile. It separates inclusive footprint/memo/resolve work and exceptional synchronous requests; no optimization or timing benefit is yet claimed. Shared compilation currently prevents applying/rebuilding it safely. Goal remains active.
