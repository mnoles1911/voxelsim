# Environment, crafting and detached-object handover — 2026-09-07

## Current continuation status

PR245 is merged as `da955d3e7d40139ce7f50e71618c497de6ffea86` (head9abebb2); all required CI passed, including MSVC12m26s, Forge15m15s and cross-compiler determinism. PR234-245 are merged.

The next source-frozen checkpoint contains private GPU publication, actual-index/absence regressions, the normal-GPU isolated visual pilot, bounded core terrain edit batches, pool Reset across travel, accurate diagnostic cohorts, and cold fine-tile fallback. Final build `build-environment-fine-fallback.log` passed 11 actions in 50.44 seconds; current core library target and UE follow-up build succeeded. `environment-final-gpu-fine-tests.log` passed all 55 DX12 tests with a complete queue and normal exit0. Focused core authority five cases passed in 0.24 seconds. Unity178cpp and frontend357-switch lints passed.

`environment-default-gpu-drain-visual-probe.log` passed normally: 27 pages,16 allocated/11 absent,8.908s yielding producer drain,5.646ms blocking boundary. First-after and steady images7AC1CA5E4C1B06A9C8FC439DFAB86343 were inspected green. This is not production latency acceptance. Removal-only pool flushing now drains pending index removals.

First real travel run `environment-gpu-world-travel.log` hit a cold fine-tile absence gate before travel. Fixed both final lock checks and restricted fallback to confirmed filesystem absence; corrupt/unreadable files remain refused. The next run `environment-gpu-world-travel-fallback.log` completed three worlds/two real OpenLevel transitions normally without allocator errors and with three screenshots, but lacked first-world completed verification samples; harness correctly refused. The longer55s/world run `environment-gpu-world-travel-settled.log` PASSED with normal exit0 and privateUserDir4c097db40e6d4295baacdaf124cb37f5. All three images were reviewed: consistent terrain after settling, with existing sky coverage warning. Acceptance now requires new per-world cross-checks, one capture per world, ordered captures/empty teardowns and direct allocator-error rejection. Screenshots show sky coverage and unfinished terrain streaming; no visual-completion claim. Root owns shared UE slot until explicit release to persistence with tested local commit.

Persistence repeatedOpenLevel log session-ui-b8cb9626822c4a67a4738476572ee5da reported22 allocator mismatches/8chunks. Root found Reset cleared CPU descriptors but retained process-global GPU buffers; new source rotates holders and epoch-tags diagnostics. CLAIM DARK separately compared delayed GPU receipts to current host counters, also present in root prior HeldGpu log; new source associates matching cohorts. Root harness now refuses allocator cross-check/doubleGrant/badFree/Error DARK. Prior20/20 page parity remains a private-pack result, not clean ordinary allocator acceptance. See environment-gpu-pool-reset.md. Full repeatedOpenLevel remains required after build.

Ignored ue-project/Saved/authority-capture-draft contains unapplied capture split patch/hash manifest. It is not compiled or integrated and still requires async coordinator credits and host lifetime/config-freeze lease. Existing WaitForInFlightTasks60s timeout can free raw-pointer-owned state after logging an error; this remains a separate unresolved teardown safety gap. Do not dispatch new capture workers under that assumption without fixing lifetime.

PR244 is merged as `12e7473fd6e31976dc8d2a99ee441151022d59ed`; all required CI passed after the narrow GCC material initialization fix. PR234-244 are merged.

Current culling/provider checkpoint passed `build-environment-culling-identity.log` (14 actions,48.62s),49/49DX12 tests and real HeldGpu20/20 allocated-page parity against unculled CPU output. Actual manifest identity reported READY providerBound=1. Conservative vertical culling preserves retained order and checked coarse/apron coordinates. Actual active-streamer provider stamps now guard replay while retaining legacy unstamped support.

Both same-site visual probes completed normally and first-after/steady images were inspected green without fallback/glow: culling capture76EC58C44E7BA36BB5F81FB869AB4328 and cache-only EC17CA46422B021AEBEAAA97D6E62C65. Culling reduced copied bytes/request from520399 to148589 and copying time92.455us to23.797us; total asset preparation did not improve (314.075us to319.377us/request). Diagnostic cache-only reduced measured asset work to78.887us/request, resolution40.131us/request, with all warm launches0. Different request mixes and window counters do not prove frame-rate or tail-latency improvement. Cache remains opt-in because epoch, memory and worker-lifetime gaps remain. See performance follow-up for complete receipts.

Root released its UE slot after the cache probe normal exit; persistence owns the next walking/recovery batch. Current checkpoint is committed9abebb2/PR245. GPU publication draft was applied after verified base hashes; next compile and execute the new tests and GPU allocator visual pilot when the shared slot is released. Production ownership remains disabled and incomplete.

PR243 is merged as `a5a60d99a2ea94ece7aaa0ed801bf9c03384a49a` (head468a8f27), with all required CI green, including cross-compiler determinism and every Asset Forge spec. Root branch fast-forwarded to that merge. The following visual checkpoint is committed.

Next verified batch: private GPU Begin/Poll/Cancel reservations and four DX12 tests, asset preparation timing splits, and stationary authority facade/coordinator. `build-environment-private-gpu-reservation-fixed.log` succeeded5actions15.13seconds after a braced render-command macro fix; `environment-private-gpu-reservation-tests.log` passed46/46DX12 tests, normalexit0. Core `build/authority-core` MSVC focused target passed5cases, including retained-generation admission. No production ownership activation exists in either increment. See private-gpu-reservations.md and stationary-asset-authority-core.md.

`environment-private-gpu-no-buffer-probe.log` passed normally with4views in `.../ProductionVisualPilot/84D735C74D71ADE8E2D30688EB9A9FB9/`. Root inspected first-after and steady: intended green with no fallback/glow, without EXR diagnostic capture.27pages19allocated/8absent;22.608ms blocking boundary is not performance acceptance. The ordinary asset timing analyzer found25windows: resolve10304.022ms, span lookup/build79.840ms, table copy/rebase4401.514ms,24,774,632,440bytes copied. These are window totals, not frame percentiles. Root released shared runtime after normal exit to the persistence task. Agent is preparing conservative vertical asset culling in ignored draft until this checkpoint is committed.

Verified checkpoint: intended material readiness now prevents first-frame fallback in the controlled real-site visual pilot. `build-environment-material-readiness.log` succeeded (11 actions, 41.58 seconds); `environment-material-readiness-tests.log` passed all42 DX12 tests with normal exit0, including changed MID, section material and fade-state rejection.

`environment-material-readiness-probe.log` passed with normal exit0. Capture directory: `ue-project/Saved/Screenshots/ProductionVisualPilot/03CEABED46B960F3669BA0AE09F29332/`. Root inspected first-after, settled and the nine-buffer contact sheet: green immediately and after settling, no gray fallback or glow. Material readiness waited0.485seconds; the isolated publication involved27pages (21allocated/6absent), with4.918ms explicitly blocking boundary. Diagnostic flush/readback means this is not performance evidence. BaseColor/WorldNormal are624x351, depth960x540; physical depth units remain uncalibrated.

The pilot remains opt-in, standalone, CPU-arena, visual-only and private-registry. Production terrain-to-interactive-object ownership remains incomplete and disabled. Full volume lighting, continuous-frame depth/shadow, default GPU allocator publication, authoritative edits/collision, restore/demotion and network ownership remain outstanding. See environment-authoritative-projection-plan.md and environment-streaming-performance-followup.md.

Earlier failures are retained in Saved logs: visual-diagnostics showed persistent darkness; initial surface-lighting overglowed; surface-exposure fixed settled brightness but retained first-frame fallback; buffer-association exposed the gray checker. The final gate checks matching complete GT/RT maps and intended render resources/proxy before preparation and again before commit. Material support compensates Unreal pre-exposure and stays off by default, enabled only with GI and propagation volumes off.

Fresh corrected material validation passed with370/369 pixel instructions and no Python/saved-material compilation failures. Its exit1 reflects existing ProjectID/GameFeatureData configuration errors, not a clean commandlet exit. Include both regenerated materials, dedicated MPC and generator scripts together.

Root checkout: D:/voxelsim/.scratch/environment-verification, branch codex/environment-persistence-integration-2026-09-07, starting HEAD bb5a5490 (merged PR242). PR234-242 are merged. Preserve shared primary D:/voxelsim and its unrelated editor. Root's probe has exited; the shared UE build/runtime slot was explicitly released to persistence task01a0799e-557f-7011-964a-db20948df98f.

Persistence task's597b22b and descendants remain publication-review blocked in that task. Do not integrate or publish them through another route. Already integrated83dba4b is unaffected. The ignored Saved/authority-draft remains unapplied and unbuilt: provider/catalog binding, preallocation bounds and explicit edit provenance require correction before use.

Next: commit this scoped checkpoint, push and merge only after required CI, then continue the production framework. Hourly heartbeat finish-environment-framework-overnight remains active; disable only when scoped work is actually complete.
## User request and checkpoint scope

The user authorized completing the remaining framework work autonomously, then requested **commit and merge all progress and prepare a handover now**. This checkpoint preserves unfinished implementation; it does not declare production environment migration complete. Resume the outstanding work below without routine confirmation.

Workspace: `D:/voxelsim`, Unreal 5.8 at `D:/UE_5.8`. Other Codex and Claude tasks share this repository and may open editors or build separate checkouts. The three environment subagents are frozen. Coordinate before writing shared files or running Unreal builds. Preserve other tasks' changes. The user authorized pushing and merging the session PR.

Read `HANDOVER-2026-09-07.md` for Asset Forge/creature work, and the persistence task's own handover for its separate session checkpoint/inventory work. Creature checkpoint `5303d99` and shared checkpoint `fa56da9` must both remain ancestors of the integrated result; use Git history for the final commit IDs.

## Implemented framework

- Asset pitches follow 100 / 50 / 25 / 12.5 mm binary subdivision. Environment sources accept 100 / 50 / 25 mm; only craftables and creatures use 12.5 mm.
- The common environment actor accepts arbitrary VXA sources and descriptors, including spec ID, kind/category, content hash, source fingerprints, seed and explicit felling capability. No four-spec restriction remains beyond legacy fixture compatibility.
- Standing actors support upright unit-scale quarter yaw with exact sign/swap coordinate transforms. Mining previews, carve, collision, trace, slope probes and save/restore agree. Arbitrary standing scale/pitch/roll are rejected; detached timber can rotate freely.
- Version 4 detached snapshots preserve IDs/revisions, immutable geometry, dynamic state, cleanup timers and tombstones. Worker compression, staged restoration, bounded paging, retry fairness and server-authoritative replication are implemented. Saved snapshots embed paged geometry; they are portable independently of the runtime page cache.
- Sparse environment actor storage now covers import, LOD construction, occupied mesh sections, edits, collision queries and snapshots. Old dense snapshots remain readable. New bodies encode sorted occupied 8-cube chunks. Limits: 131,072 chunks per grid; 262,144 per hierarchy; 8,192 mesh sections and 1,000,000 visible faces across LODs. Standard allocator OOM remains fatal in exception-disabled Unreal builds; admission failures return explicitly. See `sparse-environment-grid.md`.
- Replica motion blends displayed detached transforms over 100 ms while registry transforms immediately retain authoritative state. Standing lattice transforms, scale corrections and teleports over 10 m snap. No extrapolation. Motion updates now preserve residency instead of marking live actors dormant. The real network probe checks displayed positions and live residency as well as registry state.
- Timber ground support uses geometry/hinge/velocity bounds rather than a fixed 32 m square. Sampling and cooks are globally budgeted per world. Bodies pause with velocities/awake state preserved until resident support exists. Selective terrain-edit invalidation is implemented and wired through level-zero `MarkChunkDirtyForRemesh`, covering grouped local and replicated edits. Startup replay precedes creation of these proxies. Verify any future mutation path also invalidates support.

## Production handoff remains unfinished

Do not enable ordinary tree overlays yet. `docs/production-environment-ownership.md` remains the overall acceptance plan.

Implemented prerequisites now include held GPU generation results, adapter-owned prepared CPU/GPU buffers and a pool batch that reserves every replacement before replacing old slots. Capacity failure rolls back. That pool batch deliberately refuses the default GPU allocator mode: there is no whole-batch host-verifiable reservation there yet.

`voxelcore/assetcandidate.h` is a newly added, **not yet integrated** helper for composition-clipped VXA extraction, canonical bounds and apron-aware page intersection. Its output has canonical yaw baked into voxel indices and therefore requires an anchor-translation-only actor transform. Source provenance must retain original quarter yaw. Focused compiled tests now cover overlap suppression, composition clipping, all quarter yaws, capacity rejection and negative-coordinate page aprons. It remains unintegrated into production.

Next steps:

1. Wire canonical candidate selection from `VoxelResolveTerrainInstances`/`ResolvedAssetsForFootprint`, manifest bank IDs/kinds and stable world/provider/catalog fingerprints. Re-resolve the candidate's complete footprint so all earlier overlapping winners are included. Check terrain residency before resolving and reject edited footprints for the first pilot.
2. Integrate composition-clipped object preparation and enumerate the full affected visible, pending and parked page set at every LOD, including aprons. Preserve generated-world/collision authority.
3. **Do not simply remove owned instances from the stamp list.** That reveals cells belonging to later overlapping assets. Resolve the canonical first non-air winner and suppress its terrain rendering if object-owned. The new helper demonstrates that distinction; CPU and GPU winner-suppression support now exists and classic GPU readbacks pass, but the production request builder does not populate ownership markers yet.
4. Wire held results and atomic pool publication into WorldSubsystem dispatch/drain, generation rejection and parked adoption. Coordinate object material visibility with terrain index publication; prove frame-by-frame no holes or duplicates. A first opt-in pilot may use CPU pool allocation with real GPU generation, but default GPU allocation still needs an equivalent transaction.
5. Persist provenance and edited coarse projections for demotion, reload and multiplayer. Never recreate the original tree over an edited instance.
6. Run cross-chunk near/far, CPU/GPU, cancellation, parked adoption, edit/demote/reload and multiplayer ownership acceptance. Production foliage wind still needs integration into this route.

## Verification and practical limitations

Before the latest sparse/ground/publication changes, `Saved/build-environment-yaw.log` succeeded and `Saved/environment-yaw-tests.log` passed all 11 object tests. Previous real async save and dedicated-server/two-client evidence is documented in `detached-object-integration.md`; those results are not claims about this newest revision.

Sparse core tests passed in normal and exception-disabled targets (`Saved/sparse-asset-grid-core-tests.log`). New UE tests are `EnvironmentSparseCodec`, `ReplicaMotion`, `TimberGroundBounds` and `ProductionPageBatch`, all under `Voxel.Objects`. The initial checkpoint build found a missing third argument in the new pool test; that fixture was corrected. Final checkpoint build/test results are recorded below when available.

Large cold ground footprints can take many seconds or minutes under the conservative frame budget. Overlapping proxies duplicate sections. Heightfield ground cannot represent caves, overhangs or vertical surfaces. Fast external impulses may exceed lookahead. These limitations require a shared production collision provider; a successful unit test does not establish complete physics correctness.

The independent Python snapshot validator now recognizes sparse bodies, checking order, bounds, padding, truncation and exact encoded bytes. Sparse hashes are storage-byte hashes; do not compare them to legacy dense hashes as semantic equality.

Useful commands (do not run alongside another editor/compiler):

```powershell
& D:/UE_5.8/Engine/Build/BatchFiles/Build.bat VoxelEarthEditor Win64 Development -project=D:/voxelsim/ue-project/VoxelEarth.uproject -WaitMutex -NoHotReloadFromIDE -DisableAdaptiveUnity
& D:/voxelsim/tools/verify-object-persistence.ps1 -LogPath D:/voxelsim/Saved/environment-checkpoint-tests.log
& D:/voxelsim/tools/verify-detached-network.ps1 -TimeoutSeconds 480
```

Use DX12 for game/client validation; NullRHI clients hit unrelated water/terrain initialization faults. Test launchers create hidden processes and return owned PIDs; wait for their logs and normal exit. Do not kill unrelated editor/compiler processes. Build wrappers may report shell success even when Unreal failed: inspect `Result: Succeeded`.

## Final checkpoint verification

`Saved/build-environment-checkpoint.log`: **Result: Succeeded**, 15.17 seconds for the incremental rebuild after fixing the new test fixture. This is the D:/voxelsim environment checkpoint before merging the separate persistence branch, not verification of that later combined tree. Focused object automation was attempted but its busy-process guard refused to launch because another editor/compiler was active; the other task was left untouched. No new object automation, real multiplayer or visual fall/LOD run was performed for this revision.

Local integration `a347e83` preserves creature checkpoint `5303d99` and persistence `c3b7951`; `bfcf5e9` also merges the verified persistence compile fixes `93fcd2f`. Persistence PR #232 merged separately. Later Asset Forge commits fix plant radii, remove the rejected goblins and select the approved craft export directory; those commits are preserved in the follow-up. Current wildlife exports are visual baselines, not anatomy-approved results; another task owns their reconstruction.

Follow-up CI repairs include unique environment-local symbols, explicit frontend switch classification, narrow reviewed floating API boundaries (integer derivation remains required), portable signed karst division, a missing `<cmath>` include, and explicit SciPy/Numba bake-test dependencies. The stale terrain hash was verified against the approved `660f041` pond-floor change: reverting only 0.5 m to 1.0 m reproduces the old hash. The version/payload test passes with both old and current pins. Six independent checkpoint-store tests pass. Full new terrain numerical tests have not been run locally because pytest/Numba are not installed in this runtime.

An hourly continuation is active in this task for twelve runs: `finish-environment-framework-overnight`. It resumes the scoped work and updates evidence, preserving other active tasks. Disable it when the environment framework is complete. It is not a claim that any remaining feature has already shipped.

Isolated integration verification at 72866e1: Unreal Editor Development build succeeded (160.21 s), using a freshly compiled voxelcore.lib. All three focused CTest targets pass: ownership/candidate extraction, sparse storage, and exception-disabled sparse storage. Float-boundary, unity-collision, frontend classification and shader UB lints pass. The independent checkout is D:/voxelsim/.scratch/environment-verification; the shared Asset Forge branch and its anatomy pilot files remain untouched. Object automation is running against the isolated modules; no runtime result is claimed yet.


All 15 Voxel.Objects automation tests passed in Saved/environment-isolated-tests.log with exit status 0. Startup has separate existing FVoxelMarchCS permutation-count ensure, blank ProjectID and GameFeatureData configuration diagnostics; this is not a clean startup claim. PR #234 is open. First CI run found portable palette-comment and GCC/Clang indentation failures, now repaired; terrain CI passed 669 tests with one headwater-on-dry-cell failure under investigation.

Terrain headwater repair: 670 tests passed, 7 skipped (Saved/environment-headwaters-verified-tests.log). Product bake version29 excludes original source points dried by final surface constraints; terrain version8 and terrain fingerprint stay unchanged. Exact surviving coordinates/discharge and unchanged elevation/water-surface/discharge arrays are verified.

Render-only CPU/classic GPU/worklist support now preserves canonical overlap winners without reserving material IDs. See environment-render-suppression.md. Full frozen-layout core CTest passes all five targets (355.81s); the earlier local failure occurred while instance-layout headers changed during compilation, and the consistent rebuild resolves it. Rebuilt UE object automation passes all17 tests including actual classic GPU readbacks and admission limits. Worklist readback test source awaits its own build/run. A bounded production candidate preparation bridge is in progress; no ordinary actor ownership switch is enabled.
