# Lessons from the ring-gap night and the throughput day (2026-07-27)

Two sessions, two PRs (#160, #161), ~40 instrumented legs on real terrain.
What was learned, separated from what was built — because the lessons will
outlive the code.

## Progress, in one block

- **Ring gaps: root-caused (three stacked defects) and fixed.** Converged
  coverage holes = 0 on both meshers, repeatedly. The symptom is now a logged
  number (`voxel.Stream.CoverageVerify`) and a one-command repro, never again
  manual-only.
- **The GPU stack is the default across the board**: pooled renderer
  (`voxel.Stream.GPU`), mesh fork (opt-out `-VoxelNoGpuMesh`), D1
  direct-to-pool (`voxel.GPU.MeshDirectToPool`, byte-identical gate), frustum
  cull + multi-batch (`voxel.Stream.GPUCull`, K=1024), chunk-local emit.
- **The adopted 128 m / 4 km cascade** runs at p50 22.4 ms (was 46 ms at
  adoption — un-culled submission), pool sized from measured demand (44M
  quads), demand byte-identical across meshers and legs.
- **Vs the pre-GPU method at its own scale**: 6x fewer hitches, better
  p50/p95, equal coverage.
- **Open P0, precisely scoped**: pooled-arm loading plateaus at ~600 chunks/s
  at the 4 km cascade (component arm: ~1,080). Not the mesher, not the fork
  caps, not pipeline depth — all falsified. Diagnose the pooled apply/table
  path before building tile batching (design + occupancy census on record).

## Deprecation decision (owner, 2026-07-27)

**The CPU mesher is deprecated as a production path.** The GPU fork is the
producer of record. The CPU mesher remains in-tree, un-defaulted, for exactly
two purposes until tile batching ships and the plateau P0 is closed:
(1) it is the byte-equality REFERENCE every GPU gate compares against —
deleting it deletes the correctness argument; (2) `-VoxelNoGpuMesh` remains
the A/B control arm. New work should not extend it.

## Lessons

1. **A hypothesis is cheaper to kill with instrumentation than to fix
   blind.** The handoff's inner-hysteresis lead was the right neighbourhood
   and the wrong mechanism; one instrumented flight replaced it with three
   real defects. Every fix this session was preceded by a leg that measured
   the defect and followed by a leg that measured its absence.
2. **Convert visual symptoms into logged numbers first.** The coverage
   verifier turned "I see rings" into `holes=N`, which is what made
   root-causing, regression-proofing, and the head-to-heads possible at all.
   (And verify the verifier: its first cut misread bridged LOD stand-ins as
   holes and buried the signal.)
3. **Defaults are measurements, and stale comments about defaults are
   landmines.** The 46 ms wall existed because a doc said the cull was
   "on by default" and the code said 0. Three of this session's biggest wins
   were one-line default flips justified by legs (cull on, caps 4/8, jif 8);
   two more were REVERTED by legs (dispatch-after-drain, in-flight 1024).
   Both kinds are recorded with the measurement in the comment.
4. **Negative results are deliverables.** Six levers were measured dead this
   session (cadence under motion, batch caps at the plateau, pipeline depth,
   cascades-via-ExecCmds, K=64 and K=256 against K=1024, mesher choice
   against the plateau). Each is written down with its numbers so it cannot
   be re-tried blind.
5. **The bottleneck migrates; re-baseline after every fix.** Meshing ->
   admission churn -> retention semantics -> fork bookkeeping -> draw
   submission -> the pooled loading plateau. Five times this session the
   "obvious next fix" changed because the previous fix moved the constraint.
6. **Flat cost curves point at submission; spiky ones at churn.** renderMs
   flat-at-45 with rhiMs flat meant per-quad submission, not stalls — and
   `rhiMs` structurally cannot report GPU back-pressure (it subtracts its
   own idle), a misread that nearly sent the fix the wrong way.
7. **Watch for identity conditions in guards.** Two defects were guards that
   could never fire: the global bHeldBack return that latched every ring's
   refill flag, and `Num() <= ColdBandHeldThisFrame` when the throttle held
   the whole queue. Both read as reasonable code; both were provable no-ops
   from the log.
8. **Same-frame lifecycle races hide in "already tracked" checks.** The
   scan-before-park race (pending-unload records blocking re-admission) was
   invisible until per-chunk release logging existed. Resurrection — cancel
   the unload, reuse the geometry — was both the fix and a throughput win.
9. **PowerShell mangles embedded quotes and non-ASCII through native
   round-trips.** Two encoding incidents (Set-Content re-encoding em-dashes,
   commit -m with inner quotes) — write files with proper tooling, commit
   with -F, keep commas inside single-quoted args.
10. **Ground rules earn their keep.** "Never conclude from one run" caught
    the K=1024 spike/steady inversion; "a lull is not a finish line" kept
    settle claims honest; rule 9 (no .ush edits mid-run) was violated once,
    caught in seconds, reverted. The one rule this session adds: **when an
    agent reports "behaviourally identical", verify the control flow
    yourself** — it was true every time, but the one time it is not will be
    the expensive one.

Full data: docs/measurements/ring-gap-2026-07-27.txt and
docs/measurements/gpu-throughput-wave-2026-07-27.txt.
