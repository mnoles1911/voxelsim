# Continuation, September 10, 2026 (second session) — PR #254

Picked up from the "Final checkpoint decision" below. The four items it named as the starting point are done and landed on branch `claude/ci-linux-compile-fix-2026-09-10`, GitHub PR https://github.com/mnoles1911/voxelsim/pull/254 (commits a9a05da, 44d005f, c7dfe8d, e0a14c5). Evidence directories are under `.scratch/` and are listed per item.

1. **Linux CI compile failures: fixed.** Test code and one CMake flag only. `test_assetappearancepage.cpp` mixed the Material enum with a MaterialId in a conditional (GCC -Wextra); `test_mip_provenance.cpp` used std::sort over a bounded std::array, which trips GCC 13 -Warray-bounds inside the introsort at -O2 (replaced by an insertion sort over at most eight votes, same ordering); `vxc_mip_provenance_tests` compiled `brick.h` under -Wconversion, which clang reports as sign-conversion errors in a header the test does not own (now the same warning set as every other test target). Local clang++ Release build zero warnings, 13/13 and 11/11 pass; GitHub g++ and clang++ jobs pass on the PR.
2. **Coherent merged UE build: done.** The first merged build failed to link with two unresolved externals. (a) `FVoxelPublishedAppearanceCatalog::FindResource`: winbase.h defines `FindResource` as a macro and `VoxelPlayerRecords.cpp` includes Windows headers earlier in the same unity blob, so the definition was emitted as `FindResourceW` while every other blob called `FindResource`; same class as the `near` collision in #253. Renamed to `FindResourceId` at the declaration, definition and eight call sites, hazard recorded at the declaration. (b) `vxc::Amplifier::surfaceBoundPairMm`: the MSVC `voxelcore.lib` was stale against merged source (the build log had said so); rebuilt `build/voxel-core-msvc`, CTest 15/15. `Tools/voxel-build.ps1 -AllowDirty -Verify` then passed with "Target is up to date" (`.scratch/merged-build-2`, and again after item 4 in `.scratch/merged-build-3`).
3. **Material regeneration: done.** `ue-project/Tools/create_vegetation_materials.py` run in a rendering-enabled commandlet (-AllowCommandletRendering -dx12 -sm6) maintained MPC_VoxelSurfaceLighting in place and rebuilt `M_VoxelDetailAsset` and `M_VoxelEnvironmentLOD` from the merged appearance + surface-lighting scripts. The regen log carries 36 "Failed to compile Material" warnings, all "Custom node missing input 1 (World)" during graph assembly and none after the graphs completed (the two trailing lines are the end-of-run unique-warning replay). Independent fresh-process `validate_vegetation_materials.py`: success marker, 0 python errors, 0 material compile failures, pixel shader instructions 372 (detail) / 385 (environment LOD) against 369/368 on 7 September. Both .uasset files are committed. (`.scratch/vegetation-materials-regen-1`, `-validate-1`.)
4. **Focused ownership/appearance/descriptor runtime tests: run, one real merge defect found and fixed.** On the coherent editor with DX12: `Voxel.Objects`+`Voxel.Environment` 58/60 (`objects-environment.log`), then 60/60 after the fix (`objects-environment-2.log`); 32 parameter-free `Voxel.Appearance`/`Voxel.Detail.CacheIndex`/`Voxel.Ecology.RouteParserSideEffects` tests pass (`appearance.log`); targeted rerun of GeneralEnvironment, ComposedActor, EditableActor and GenericOpaqueRestore 4/4 with `-VoxelAssetAppearanceDir=<fresh private dir> -VoxelEnvironmentLODValidate` (`targeted-2.log`, JSON report in `report-4/`). All under `.scratch/merged-automation-1/`.
   - The defect: the checkpoint had widened `AVoxelEnvironmentLODPrototype` to accept 12.5 mm environment sources at initialisation and in both saved-grid decode paths. ADR-0010 (updated 6 September) reserves 12.5 mm for craftables and creatures; main's `Voxel.Objects.GeneralEnvironment` encodes that and failed. Restored main's 25/50/100 mm sets (the tree-felling axe check is an entity and untouched). The checkpoint's `Voxel.Appearance.GenericOpaqueRestore` used a 12.5 mm environment fixture whose staged restore failed immediately on the merged build; the case is now 50 mm (two hierarchy levels) and the completion assertion names the item. Why the 12.5 mm staged restore itself failed was not root-caused: the path is now unreachable for environment sources by policy, and the 25 mm and 100 mm items exercise the same code and pass.
   - Not run here because they need harness parameters: `DetailLodRealAssets`, `DetailSizeCullRealAssets`, `DetailPersistentAuthoredLODs`, `RealTree`, `Voxel.Ecology.PublishedLibraryIntegration`. Automation warnings "UWorld::DestroyActor: World has no context" appear in several passing tests and do not fail them.

Nothing else in this document changes. No performance claim is made; the invalidated culling A/B (24/25), mesh retirement, R0 diagnostics, cache rebake and gameplay acceptance remain outstanding exactly as listed below. Two notes for the next step: `docs/handoff-patches/r0-entry-profile/r0-entry-profile.patch` no longer applies to merged main (`VoxelWorldSubsystem.cpp` hunk near line 20013 moved) and needs a rebase; `docs/handoff-patches/detail-retirement/retirement.patch` still applies cleanly. Cache identity: `VoxelAssetAppearance.cpp` is on the identity list and was merged, so `temperate-authored-lods-full-1` must be re-verified against the merged builder identity before any A/B; the material regeneration does not touch cache identity.

Helper scripts used this session (scratchpad only, not committed): a generic UE automation runner cloned from `Tools/verify-object-persistence.ps1` accepting a '+' filter list, `-ExtraArgs` and `-ReportExportPath`; and a python-commandlet wrapper that judges on the `VEGETATION_*` markers, `LogPython: Error` and "Failed to compile Material" counts rather than exit code.

---
# Final checkpoint decision — PR #253

The user explicitly directed immediate commit/merge/push on September10 after reauthorizing the GitHub destination. Further validation was stopped; this is a work checkpoint, not full runtime or performance acceptance. Integration branch: `codex/temperate-checkpoint-integration-2026-09-10`; GitHub PR: https://github.com/mnoles1911/voxelsim/pull/253. The original checkpoint is df355dc; subsequent integration/fix commits include d9f4d90, c965170 and efa9f3d. Earlier STOPPED and unmerged descriptions below are historical and superseded by this decision and the PR's actual merge status.

Verified: Asset Forge frontend build; 37 Python analysis regressions; 11 focused native CTest targets; Asset Forge quick selftest after regenerating categories; float, unity, frontend-switch and shader lints. GitHub shader compilation, terrain-service, Docker and ancillary lint jobs passed.

Outstanding at the immediate-merge instruction: Linux CI still fails compilation in focused tests (GCC reports small-array sort bounds in mip provenance and enum/non-enum conditional in appearance-page tests). Full-library generation/MSVC checks were not all complete. The first merged UE build found old save API callers and a Windows `near` macro collision, both corrected in committed source. Subsequent compiler attempts stalled; the direct no-PCH probe consumed CPU but was stopped at the user's immediate-finish instruction. No final merged UE build or merged GPU runtime pass is claimed. Both conflicted material assets retain the checkpoint versions; regeneration from merged appearance+surface-lighting scripts remains required. Private caches need rebaking after material/source identity changes.

All feature/performance follow-ups below remain outstanding, especially invalidated culling A/B24/25, mesh retirement, R0 diagnostics and gameplay acceptance. Do not interpret merge completion as those goals being achieved. Future work should begin with the CI compiler failures, a coherent merged UE build, material regeneration, and focused ownership/appearance/descriptor runtime tests.

---
# STOPPED — latest handoff update, September 10, 2026

**The user again explicitly ordered all work and subagents stopped. Do not resume implementation, builds, merge resolution, commits, or pushes without a new instruction. This section supersedes earlier continuation/stop-state text below.**

## Exact final state

- All three agents (`cpu_stream_cost`, `frame_stalls`, `gpu_cost`) were interrupted again. No owned UE, compiler, CMake, or MSBuild process was present in the final process inventory.
- The main checkout `D:\voxelsim` remains on `codex/asset-forge-session-completion-2026-09-07` at **df355dc**, a local checkpoint commit: `Checkpoint temperate forest generators, appearance, ecology and performance handoff` (4,963 files). The original feature work is preserved there.
- **No merge was completed.** An isolated worktree at `D:\voxelsim\.scratch\temperate-checkpoint-integration`, branch `codex/temperate-checkpoint-integration-2026-09-10`, has a merge of `origin/main` in progress. Preserve this worktree and its index/working files. It contains partial, uncompiled conflict resolutions. Do not mistake conflict-marker removal for validated integration.
- Root's attempted remote push was rejected by automatic approval review because it required explicit authorization to upload source/assets to GitHub. No push by this agent succeeded. The final inventory observed a separate `git push --porcelain origin` process chain (PID22484/25244 and helpers), whose command was not issued by this agent; it was left untouched. Remote state is therefore not verified, and must not be inferred from this local checkpoint.
- This latest handoff update is deliberately **uncommitted**, honoring the renewed stop instruction. The same updated document is copied into the integration worktree for discoverability. No further staging or merging was done.

## Validation performed before the integration attempt

- Asset Forge `npm run build`: PASS (TypeScript + Vite).
- Python analysis tests: **37 PASS**, with `PYTHONPATH=asset-forge/tools;asset-forge;.scratch/oak-python` to provide existing SciPy. Initial run without SciPy had two import errors; corrected dependency run passed.
- Rebuilt and ran five focused native targets in the ORIGINAL checkout: asset appearance page, ecology, bank, manifest, ownership. **69 PASS**, no failure markers. These results do not validate the later integrated source.
- Logs: `.scratch/handoff-python-tests.txt`, `.scratch/handoff-web-build.txt`, `.scratch/handoff-native-build.txt`, `.scratch/handoff-native-tests.txt`.
- `.gitignore` now excludes root scratch dependencies/worktrees and regenerable `ue-project/Content/Voxel/AppearancePilot/` and `Generated/DetailPreview/`. Approved library/source/frontend assets are in the checkpoint; private UE cache imports remain local.
- Unapplied diagnostic/retirement patches were preserved in tracked `docs/handoff-patches/r0-entry-profile/` and `docs/handoff-patches/detail-retirement/`, including README and source hash files. They remain unapplied feature proposals, not tested runtime changes.

## Partial integration details for a future agent

The fetched main had74 commits beyond the checkpoint's original parent. A merge preview found22 conflicted paths. Root resolved/staged these in the INTEGRATION WORKTREE ONLY:

- `docs/production-environment-ownership.md`: combined applicable appearance and upstream ownership notes.
- `M_VoxelDetailAsset.uasset`, `M_VoxelEnvironmentLOD.uasset`: selected checkpoint's approved appearance materials. Combined material-script/runtime compatibility still needs validation.
- `VoxelAgentSubsystem.cpp`: preserved both tick profiling and upstream checkpoint readiness gate.
- `VoxelEnvironmentLODPrototype.cpp`: retained both appearance/profiling and render fence includes; preserved visual-only-preparation guard and composition guard. Semantic validation of upstream preparation versus composed appearance remains required.
- `VoxelProductionEnvironmentAdapter.cpp/.h`: retained both GPU readback quads and validated-absence evidence; preserved strict complete-page validation and absence checks.
- `VoxelProductionEnvironmentAdapterTests.cpp`: combined empty-page/readback tests and upstream absence tests.
- `VoxelEnvironmentAsset.cpp/.h`: both branches had assigned schema2 to different payloads. Resolution preserves MAIN schema2 provenance layout and introduces schema3 for composition metadata plus optional provenance; schema1 and legacy remain unchanged. Old unpublished checkpoint schema2 composed saves are not migrated. User previously allowed development-save regeneration. This resolution is UNCOMPILED/UNTESTED.
- `VoxelEnvironmentAssetDescriptorTests.cpp`: updated expected composed bytes to schema3 and added combined composition/provenance round-trip and truncation tests.
- `VoxelEnvironmentProvenanceTests.cpp`: unknown-version test moved from3 to4. Existing schema2 tests retained.

The final Git inventory still reports these12 paths **unmerged in the index**, even where agents may have removed working-file markers:

1. `ue-project/Shaders/VoxelAssetStamp.usf`
2. `ue-project/Shaders/VoxelWorklist.ush`
3. `ue-project/Shaders/VoxelWorklistAssetStamp.usf`
4. `ue-project/Source/VoxelEarth/VoxelSkySubsystem.cpp`
5. `ue-project/Source/VoxelEarth/VoxelWorldSubsystem.cpp`
6. `ue-project/Source/VoxelEarthShaders/Private/VoxelGpuMeshJobManager.cpp`
7. `ue-project/Source/VoxelEarthShaders/Private/VoxelGpuWorklist.cpp`
8. `ue-project/Source/VoxelEarthShaders/Private/VoxelGpuWorldGen.cpp`
9. `ue-project/Source/VoxelEarthShaders/Public/VoxelGpuWorklist.h`
10. `ue-project/Source/VoxelEarthShaders/Public/VoxelGpuWorldGen.h`
11. `voxel-core/include/voxelcore/assetfield.h`
12. `voxel-core/tests/CMakeLists.txt`

Last agent reports, **not accepted build/test evidence**:

- CPU agent intended to combine ecology setters/resolution with upstream configuration revisions and bounded cache; copy constructor/assignment must retain ecology state. Every `setEcology` attempt that clears state must invalidate revision, including failure. Upstream ordinary-site work bound does not account for ecology competition halos; residency must use full sampling reach. Agent had been asked to build isolated native tests but final inventory showed no active build; no completion evidence received.
- World agent reported conflict-marker-free World source, not staged: shared request builder carries appearance output/EditEpoch with ownership generation/PageLease; bounded cache replaces TMap; predictive tasks globally capped at8. Held publication must preserve first-winner suppression and appearance identity. Full semantic review and runtime tests pending.
- GPU agent planned to retain public `RenderOwned` and `SuppressTerrainRender` inputs, OR them into the existing48-byte worklist flag and shared classic first-winner clear path, avoiding two redundant masks. This must preserve upstream ownership callers and ours; no completed validation reported. Inspect actual partial edits rather than assuming the plan was fully executed.

## What remains stopped

Do not finish the merge merely because the prior goal remains active. A future authorized continuation should first inspect the isolated merge state and all partial edits, then complete conflict resolution and fresh merged native/UE tests before any merge claim. The broader unfinished performance/ecology work and invalidated24/25 comparison are detailed below. No new performance gain or overall completion is claimed.

---
# Temperate forest placement and rendering — agent handoff

Prepared September 10, 2026. User explicitly stopped all work and requested this single handoff. Resume only when the receiving agent is instructed to do so. This document supersedes optimistic/in-progress wording in older status entries.

## Stop state

- All three subagents have been interrupted or were already complete: `frame_stalls`, `cpu_stream_cost`, `gpu_cost`.
- No owned UE capture or build was running at the last confirmed state. Last attempted owned build (`Tools/voxel-build.ps1`, log directory `.scratch/ecology-build-68`) refused to start because another session was compiling.
- Another session's `dotnet` PID19120 was still observed immediately before the user's stop. Earlier that build also had `cl` PID9340. Do not kill another session's processes. Recheck current processes before any build/capture; these PID observations are historical.
- A final read-only process/file inventory was attempted after stop but could not launch: `helper_unknown_error: apply deny-read ACLs`. Therefore current external processes and any last analyzer files from the interrupted agent are unverified.
- No goal was marked complete. Do not resume automatically merely because the previous goal remains active. No commits were made by this handoff work. Shared checkout contains other sessions' changes: preserve them.

## User intent and accepted product decisions

Build a reusable initial-world-generation placement system, using temperate forest as the vertical slice, later extensible across biomes/world. Natural regional communities, mixed stands, nonuniform tree sizes/spacing, canopy-dependent ground cover and hydrography/topography influence. Ancient groves, large specimens and dense thickets intentionally more common than nature. Preserve plausible sightlines, movement and building openings. No full ecological simulation. Development saves/player edits may be overwritten.

Asset Forge is the authoritative generator/library workflow: species has baseline parameters and reference instance; numbered seeds produce variants; endorsement publishes variants to the game library. Rejections/endorsements persist. Do not autoendorse private test assets.

Trees use 100mm voxels in this slice; smaller detailed understory uses 25mm. Tree voxel destruction remains relevant; user explicitly accepted understory as instanced meshes for now. All appearance is spring. User approved leaf-only cutouts and natural but distinguishable species colors, with subtle per-voxel/per-face bark and foliage variation extended across all slice assets. Approved appearance updates should reach existing variants without changing endorsement status.

Current active engineering focus: reduce CPU/GPU milliseconds and verify tree/understory LOD, culling, source residency and streaming. User authorized subagents and UE/GPU work, but now explicitly stopped work for handoff.

## Workspace and tools

- Repository: `D:\voxelsim`; shell PowerShell.
- Project: `D:\voxelsim\ue-project\VoxelEarth.uproject`.
- UE executable: `D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe`.
- Python: `C:\Users\Matt Noles\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe`; set `PYTHONPATH=asset-forge/tools` for tests.
- Native build: `build/voxel-core-msvc`, library `Release/voxelcore.lib`. Do not accidentally use secondary `voxel-core/build`.
- CMake: `C:\Program Files\CMake\bin\cmake.exe`.
- Build wrapper: `Tools/voxel-build.ps1 -AllowDirty -Verify -LogDir <fresh path>`. Verify requires a second build saying `Target is up to date`; a single successful link is insufficient.
- Permissions changed at the end to restricted filesystem/network with workspace writes allowed. Exec launcher currently failed even for read-only inventory; diagnose environment access before assuming commands ran.
- No applicable AGENTS file was found earlier; receiving agent should inspect current instructions.

## Strongest verified results

### Authored understory LODs fixed and rebuilt

A real bug was discovered: persistent mesh creation inherited Unreal reduction defaults for each new source LOD (`PercentTriangles=.5^L`, base LOD0), causing authored distant LODs to be replaced by generic reductions. Transient preview creation did not do this.

Implemented in `ue-project/Source/VoxelEarth/VoxelDetailAssetSubsystem.cpp`:

- Reset reduction settings for each source model and set `ReductionSettings.BaseLODModel=L`.
- After compilation, fail closed on wrong render LOD count, authored-versus-built triangle mismatch, or active reduction.
- Success proof log: `DetailAuthoredLOD preserved mesh=... lods=N authored/builtTriangles: L0=x/x ...`.
- Canonical cache settings in subsystem and bake commandlet now contain `;authoredLOD=1`.
- Real persistent-builder regression `Voxel.Appearance.DetailPersistentAuthoredLODs`: four actual 25mm source profiles; passed DX12, zero warnings/failures in `detail-authored-lod-tests-1`.

Last coherent owned runtime was **build67-retry** (second build up to date). Another session subsequently replaced current DLLs, so filesystem binaries are no longer that baseline. Rebuild coherently before new comparisons.

Correct immutable private cache:

`ue-project/Content/Voxel/Generated/DetailPreview/temperate-authored-lods-full-1/detail-bake.json`

- Manifest SHA256: `4fc65fadb332430459270fc77c8710ff80f522963d663f574ac3f45a0f5b8b36`.
- Builder identity: `065d0f4d8c5d128ec5b19ca6027e3c8584b84c77f8659c08dcfa1d2264b09be3`.
- Schema2; 339 unique preview meshes, 113 understory profiles, all 25mm.
- Bake and fresh-process verification both passed; evidence under `asset-forge/out/ecological-placement/detail-authored-lod-bake-1` and `detail-authored-lod-verify-1`.
- `analyze_authored_lod_bake.py` binds each unique manifest path to authored/built triangle proofs. Compared with old cache: same VXA/VAC, species, pitch, LOD0 and LOD count; 323 models' distant geometry changed. Library last-LOD triangles 2,782,931 → 4,157,030. This is not scene triangles or VRAM.
- Old `temperate-cull-full-1` and `temperate-attributes-full-1` caches are stale for current builder and their performance does not establish performance of corrected geometry.

Cache identity depends on `VoxelDetailAssetSubsystem.cpp`, `VoxelDetailMeshLOD.h`, `VoxelDetailWindBounds.h`, `VoxelAssetAppearance.cpp`, `VoxelBakeDetailMeshesCommandlet.cpp`, and `VoxelMeshAttributeFingerprint.h/.cpp`. Changes require another immutable bake. WorldSubsystem and diagnostic test inlines do not affect cache identity.

### Actual material/geometry size-cull validation passed

Evidence: `asset-forge/out/ecological-placement/detail-size-cull-real-tests-3` and `detail-size-cull-real-capture-3`.

- 128 cached-versus-transient comparisons, exact color and mask equality; zero warnings/failures.
- All 32 outside-distance captures empty; all 96 positive visibility controls visible.
- Four seeds: meadow-daisy0007 (32m), meadow-grass0007 (48m), water-reed0007 (160m), bramble-thicket0007 (208m).
- Perspective FOV50/90, wind off/on, fixed time1.3, wind(8,2,1); 256 PNGs.
- All128 material checks ready, no fallback; all four near cases visibly changed with wind.
- Start/end module and cache hashes matched. Agent visually reviewed representative images.
- Scope is four-seed isolated render parity/culling. It does not establish gameplay popping, all-species quality, lighting, shipping performance, or memory release.

`detail-presentation-analysis-2/report.json`: last-LOD median1496, p90 45968, max189640 triangles. Aggregate buffer estimates and equal-weight area estimates are proxies, not measured GPU residency or savings.

### CPU improvements already implemented/tested

1. `voxel-core/include/voxelcore/assetgrid.h`, `src/assetgrid.cpp`, `test_assetgrid.cpp`: cache solid count during decode so `solidCount()` is O(1). Preserve failed parse, partial malformed run count, copy/move/self-move semantics. Twelve native tests passed, including dense and real fixtures; primary native library rebuilt. UE preparation test passed. Before this, appearance validation rescanned all material runs per instance.
2. `assetappearancepage.h`: bounded8×8 XY candidate index when at least8 candidates, original candidate ordering and unapproved occluders preserved, 65536-reference cap falls back safely. Eleven native differential tests passed, including packed GPU words and failure atomicity.
3. `VoxelTerrainAppearancePage.cpp/h`: detailed substage local stats on all threads, CSV only on game thread to avoid ambiguous duplicate display names. Parser refuses ambiguous primary metrics and excludes duplicate nonprimary columns with diagnostics.

These are implemented; do not invent isolated performance gains from unmatched captures.

## Critical invalidated performance comparison

Full256m pair:

- OFF: `asset-forge/out/ecological-placement/walk-capture-24-full256-cull-off`, wrapper terminal0, eight movement checks passed.
- ON: `.../walk-capture-25-full256-cull-on`, eight movement checks passed BUT wrapper terminal1: **runtime module changed during walking test**.
- Both used corrected cache, LOD enabled, predictive off, private full-forest-low-cover-1 scene, spawn(-154740,-81476), 256m detail ring. Initial123005 understory instances,5028 groups,240 shared meshes,0 runtime mesh build ms.
- Earth DLL start hash: `EED503C0CD3BDF10AD7E56A88A598F52114991794C064299C65D4DB617FF678A`.
- Observed changed hash: `9F9441BA481662BAB6900FC2E26C9C7D470F41BEE3757BBA6168045D3C345217`; Earth/UI modification time 02:28:31 AM local during ON capture.
- Shader DLL remained `B02185DD01B992F8A869DA22BCC56CAA2E70C9943D0170AF80ED9F7C9F6FC712`.
- Another session launched headless loading-screen UE jobs and builds, even though the visible editor was closed.
- Comparator was mistakenly run after wrapper failure because it only checked start pins. Its output has been explicitly invalidated (`valid:false`) with explanation. **Do not report the apparent ~50% GPU reduction as accepted evidence.** Do not reconstruct fake passing receipts for these historical runs.

Baseline24 alone provides useful stall evidence, in `cpu-hitch-analysis.json`:

- R0 entry314.79ms across201 footprints, recompute320.97ms, streaming tick329.63ms; corresponding game-thread413.67ms. CSV FrameTime is previous logical frame (next row413.59ms).
- A separate tick had submit237.87ms, native resolve206.31ms. Do not add maxima from different ticks.
- Appearance peak83.97/74.42ms in walk/sprint; canonical52.13/46.21, pack22.27/19.44, winner8.46/7.14, resource only0.175/0.136ms. Resource validation is no longer dominant.
- GPU manager tick max3.80/3.93ms, detail tick7.86/14.43ms.
- CPU tails still limit performance even if draw culling helps GPU.

## Last changes made before stop: fail-closed capture receipts

`Tools/ecological-walk-validation.ps1` now:

- Refuses initial UE/compiler/dotnet competition; includes Earth, Shaders and UI DLL start hashes.
- Writes `run-validation.json` with running/passed/failed status and manifest binding.
- Monitors other UE/compiler processes and module size/mtime once per second; fails own capture without killing other sessions.
- Ongoing monitor deliberately excludes ShaderCompileWorker and dotnet because engine startup uses ValidatePlatforms; initial guard still checks dotnet. This is an observation guard, not an OS-wide exclusive lock.
- Finally terminates only its owned process if needed and records exit code, end runtime/input hashes, log/CSV hashes and failure.
- Existing stability, completion and end-hash checks remain.

New `asset-forge/tools/validate_walk_receipt.py` requires passed schema1 receipt, exit0, no recorded failure/competition, actual manifest SHA, exact three required module names with matching start/end hashes, matching input end hashes and actual log/CSV hashes.

`compare_size_culling_ecological_walks.py` calls receipt validation before analysis and no longer implicitly trusts launch-wrapper end checks.

New `test_walk_receipt.py` covers missing/running/failed receipt, nonzero exit, competing process, manifest change, missing/end-changed DLL hashes, input omission and artifact tamper. Combined with existing size comparison tests: **four tests passed**. PowerShell AST parse passed. No real capture has yet exercised the new wrapper.

Potential robustness follow-up: finally-block hash IO can fail, leaving original running receipt; comparator rejects that safely. Receipt status may be set passed just before final hash collection; independent comparison checks end hashes, so cannot accept mismatch. Receipt is a local validation record, not cryptographic attestation against malicious editing. Other route wrappers/comparators retain older protocols and need explicit scope or receipt migration when used for new claims.

## Staged work, NOT applied or compiled

### R0 CPU diagnostic

`.scratch/r0-entry-profile/r0-entry-profile.patch`, `README.md`, `source-sha256.txt`.

- Applicability passed with `git apply --ignore-space-change --check` against then-current CRLF source. Source SHA was `35b569ea84bea6c30288b2670b68b81fed4373a8885598c294f0b6113766dc15`.
- Opt-in `-VoxelR0EntryProfile`, with existing `-VoxelRecomputeCensus`.
- GT-only inclusive timers: Entry, Footprint, Memo, Compute, Resolve, Sky, Nearest, exceptional RequestFootprint/Prefetch. Counters for Z cells, evaluations, memo hits and epoch invalidations. RAII handles split slices and early exits. No per-Z clocks.
- Five-second maxima may be from different slices; never sum them. CSV uses accumulation for split slices.
- Cold Compute resolves exact asset instances per XY memo fill; admission repeats same-XY residency checks across Z chunks. These are candidates, not proven optimizations. Any memoization must preserve residency epochs and request/error semantics.
- Need compile, enabled capture, unique/nonnegative CSV and hierarchy validation; diagnostic overhead comparison before relying on fine differences.
- Agent was asked to add a Python analyzer and tests for these stages in new files only. Agent was interrupted by stop; completion/file inventory could not be checked. Inspect for partial new analyzer files before proceeding. No live WorldSubsystem patch was applied by root.

### Understory mesh retirement pilot

`.scratch/detail-retirement-pilot/retirement.patch`, `source.sha256`, `README.md`, proposed full source. Unapplied/uncompiled.

- Opt-in `-VoxelDetailRetireUnused`; opportunistic, not hard memory cap.
- Max32 probes/2 retire keys per tick,30s unused grace,1s rescan, bounded maps/task counts.
- Check all tracked producer tasks complete BEFORE queue-empty snapshot to avoid late producer race.
- Require all results/cache checks/loads/pending geometry/fallback queues drained; active groups, dirty/nonzero HISM, mesh compilation, async HISM build or incomplete tree pin resources.
- Remove component roots and cache aliases only once safe; clear GeometryKnown under barrier; suppress new resolution that tick; no forced synchronous GC or rendering flush.
- Five-second telemetry after convergence. No claim that removed references immediately free GPU bytes.
- Staged tests: real lifecycle/root/alias handling, real async HISM build, event-controlled producer barrier with undrained queue cases. All unrun.
- Needs compile/test, real outbound/revisit transform/seed/digest equality, teardown and late fallback testing. Continuous activity can starve retirement; not a guaranteed cap.
- Applying this touches cache identity and requires rebaking. Prefer finish corrected-cache size-cull A/B before applying retirement.

## Rendering and residency facts

- Trees are represented in voxel cascades:100mm roughly0–64m, doubling outward to12.8m at4–8km. Behind-camera radial residency is intentional for movement/turns; screen rays and opaque depth determine draw work. Current page stats combine terrain/trees, so do not claim separate invisible-tree voxel totals.
- Understory instanced meshes default256m ring, unload groups beyond1.15×radius. Default geometry has one LOD unless experimental `-VoxelDetailMeshLOD` is enabled.
- Opt-in `-VoxelDetailSizeCull` uses wind-expanded bounds and transform-aware size: end=min(ring,max(min(32m,ring),ceil(128×max(height,.5×max(width,depth))/16m)×16m)). Malformed bounds fall back to full ring. Cached/transient paths share installation policy.
- Start distance85% does not prove visible fading; no material fade claim. Gameplay pop acceptance is still required.
- Size culling affects drawing, not placement ring, source banks or retained shared meshes.
- Source banks lazily load all valid variants of a species on first access and retain until reconfiguration. Walk23 loaded487 files,104021866 bytes (~99.2MiB source grid footprint), not VRAM.
- Shared mesh UObjects/HISM/GeometryKnown can remain after groups unload; staged retirement addresses this.
- Normal stale completed groups are rejected before admission; fallback promises remain protected. Actual rapid revisit coverage still needed.

## Placement/navigation validation already available

Current private fixture: `asset-forge/out/ecological-placement/previews/full-forest-low-cover-1`. Private487 variants across162 profiles (49 trees +113 understory). Production had only21 endorsed oak/birch variants at last review. Keep these inventories distinct.

Algorithmv2 uses384m communities,72m stands,160m feature field with bounded sites up to36m radius, ancient240pm/thicket300pm and8 communities, deterministic bounded spacing/canopy/hydro logic. Fixture adjustments raised tree selection density and reduced51 dry understory weights by4, favoring20 low grass profiles; wet/tree profiles otherwise preserved.

- Hill capture16: ~42m relief, steep terrain suppresses placement.
- Shore capture15: ~11.13% water,121 reeds all within8m of water, no trees in water deeper than300mm.
- Terrain provider `terrain-diffusion-unlabeled-80b9ca451a23eae4-b5e821e98`, seed20260719 / hex000000000135276f, s16. Only center(-11,-6) hydro halo reconciled; eight other tiles not yet.
- Hill route6 passed9/9 arrivals,169.740m,143 grounded samples,0 waiting,9.3m height difference. Route7 passed9 arrivals, one ungrounded sample, no waiting; combined build scope, not isolated optimization A/B.
- Original route5 collided with hawthorn-scrub0014 leaf cells. West detour preserved original seven targets and passed; do not disable collision just to pass.
- Route4 foundation endpoint:10/10 checks,173.690m;5×5m pad2500 samples known/dry/clear,1m finite bearing, estimated6.201m³ fill needed. No fill placed, not broad build-space acceptance.
- `docs/temperate-route-hill-revisit-pilot.json`: unrun18-point344.535m outbound/reverse/spawn route, SHA57944c02bb9a006eb2c349ffa5c003bd612632931e73514070063ca21800b0de.48m diagnostic unload scope; reverse connectivity/resource restoration unverified.
- `docs/temperate-route-shore-survey-pilot.json`: unrun7-point135.47m route, spawn(-153936,-81480), SHA455191905752a52281f26f2bc67b33f6c3ec52c0400696c4f1c4802643ea43a6. Sparse samples do not prove continuously dry path.
- Hydro footprint live-channel bug fixed/tested earlier; wet-edge/dry-center live export remains pending.
- Predictive prewarm22/23 gave mixed results; stays opt-in/off by default, old LOD payload limits interpretation.
- Lighting differences remain: March hemisphere emissive/sun wrap not equivalent to detail materials. Need controlled actual lit comparison; not fixed by culling.

## Recommended continuation order

1. Re-establish shell access and inspect shared workspace/agent partial outputs. Coordinate a clear UE/build window with the other session; a closed visible editor does not imply no headless work. Do not kill unrelated jobs.
2. Review the receipt changes; run Python regressions and PS syntax parse, then coherent UE build. Freeze cache/source/fixtures/modules during matched tests.
3. Run fresh OFF/ON full256m captures with corrected cache, identical settings and fresh directories. Use `Tools/ecological-walk-validation.ps1` with `-AssetDirectory asset-forge/out/ecological-placement/previews/full-forest-low-cover-1 -SpawnAt '-154740,-81476' -DetailRingMeters 256 -DetailMeshLOD -MarchDispatchIdentity -AllowPreviewDetailCache -DetailMeshCache ue-project/Content/Voxel/Generated/DetailPreview/temperate-authored-lods-full-1/detail-bake.json -TimeoutSeconds 2400 -Output <fresh>`. Add only `-DetailSizeCull` for ON. Both receipts must pass before comparator. Each historical run took several minutes to settle/complete; do not restart healthy jobs because a tool wait expires.
4. Apply/review R0 diagnostic on compatible source, build and run separate profile capture. Identify actual cold resolve/recompute costs before changing semantics; do not mix this binary change between A/B arms.
5. Review/apply retirement pilot, compile real lifecycle/async tests, rebake new identity cache, and run controlled unload/revisit comparisons. Quantify resource counts plus actual memory rather than inferring from source voxel count.
6. Complete gameplay camera turns/cull popping, lighting parity, multiseed hill/shore navigation, understory sightline/build availability and runtime/cooked cache validation. Private fixture success is not endorsed production completeness.
7. Report measured CPU/GPU timing distributions and scope honestly. Do not mark overarching goal complete until remaining placement and representative runtime acceptance is done.

## Useful existing files

- `docs/temperate-ecological-placement-status.md`: long chronological ledger; latest append invalidates24/25 and records receipt tests. Older LIVE entries may be stale.
- `docs/environment-residency-audit.md`: rendering/residency audit and same invalidation append.
- `docs/understory-distant-lod-pilot.md`: old triangle stats explicitly superseded.
- `docs/temperate-slice-acceptance.md`: older asset/render milestones, not overall ecological completion.
- `asset-forge/tools/analyze_ecological_walk.py`, `analyze_ecological_route_frames.py`, `compare_ecological_route_frames.py`, `analyze_authored_lod_bake.py`, `compare_size_culling_ecological_walks.py`, new `validate_walk_receipt.py` and tests.
- `Tools/verify-detail-mesh-bake.ps1`: fresh-process source/attribute/material validation; does not establish cook or pixel parity by itself.

The immediate deliverable is this handoff. All further implementation/testing is intentionally stopped at the user's request.

## Versioned checkpoint follow-up

The user subsequently authorized committing and merging this checkpoint. Unapplied patches are now preserved under `docs/handoff-patches/detail-retirement/` and `docs/handoff-patches/r0-entry-profile/`; use these tracked copies on a new checkout. Original scratch files remain local. Private UE preview imports/caches and scratch dependencies are excluded from Git and must be regenerated. Asset Forge frontend production build and all37 Python analysis regressions passed at packaging time. This checkpoint does not change the outstanding acceptance limits above.
