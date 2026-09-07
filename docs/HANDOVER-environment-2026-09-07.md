# Environment, crafting and detached-object handover — 2026-09-07

## User request and checkpoint scope

The user authorized completing the remaining framework work autonomously, then requested **commit and merge all progress and prepare a handover now**. This checkpoint preserves unfinished implementation; it does not declare production environment migration complete. Resume the outstanding work below without routine confirmation.

Workspace: `D:/voxelsim`, Unreal 5.8 at `D:/UE_5.8`. Other Codex and Claude tasks share this repository and may open editors or build separate checkouts. The three environment subagents are frozen. Coordinate before writing shared files or running Unreal builds. Preserve other tasks' changes. No push was requested.

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

`voxelcore/assetcandidate.h` is a newly added, **not yet integrated** helper for composition-clipped VXA extraction, canonical bounds and apron-aware page intersection. Its output has canonical yaw baked into voxel indices and therefore requires an anchor-translation-only actor transform. Source provenance must retain original quarter yaw. The helper has not yet received a focused compiled test.

Next steps:

1. Wire canonical candidate selection from `VoxelResolveTerrainInstances`/`ResolvedAssetsForFootprint`, manifest bank IDs/kinds and stable world/provider/catalog fingerprints. Re-resolve the candidate's complete footprint so all earlier overlapping winners are included. Check terrain residency before resolving and reject edited footprints for the first pilot.
2. Integrate composition-clipped object preparation and enumerate the full affected visible, pending and parked page set at every LOD, including aprons. Preserve generated-world/collision authority.
3. **Do not simply remove owned instances from the stamp list.** That reveals cells belonging to later overlapping assets. Resolve the canonical first non-air winner and suppress its terrain rendering if object-owned. The new helper demonstrates that distinction; GPU winner suppression is still missing.
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
