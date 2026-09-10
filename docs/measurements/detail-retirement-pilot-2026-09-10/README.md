# Understory mesh retirement pilot, first run, 2026-09-10 (route captures 8 OFF / 9 ON)

Binary: branch `claude/detail-retirement-pilot-2026-09-10` (PR #254 head + R0 diagnostic +
the staged retirement patch), coherent `voxel-build.ps1 -Verify`. The patch touches a cache
identity source, so a third immutable bake `temperate-authored-lods-full-3` was made and
fresh-process verified (339 models; geometry identical to `-full-1`/`-full-2`).

Automation on that binary (`automation-run-1.txt`): 8/8 pass, including the three staged
regressions `DetailRetirementLifecycle`, `DetailRetirementAsyncHism` (real async HISM build
pins retirement, then retires after completion) and `DetailRetirementProducerBarrier`, plus
`DetailMesh`, `DetailMeshLOD`, `DetailStaleResultAdmission`, `DetailWindBounds`,
`Detail.CacheIndex`.

Route: `docs/temperate-route-hill-revisit-pilot.json`, the 18-point 344.5 m out-and-back
designed for the 48 m diagnostic ring, run for the first time. Both arms: 18/18 arrivals,
median 2.2 m/s, 292/294 and 296/297 grounded samples, zero waiting. Retirement does not
change navigation. Frame medians identical (25.5 vs 25.9 ms frame, 13.5 ms GPU both), as
expected because:

**The ON arm retired nothing.** Final telemetry: probes=30,623 keys=0 objectsUnrooted=0
deferredTicks=163 hismDeferred=1,282 blockedSeconds=0 meshes=96 cachedResources=96
roots=192 components=96. The drain barrier was not the blocker (blockedSeconds 0 after the
first window); every unused candidate was pinned at the HISM check. The pilot README
predicted this: `ClearInstances` sets the tree out of date and the conservative pilot never
asks for a rebuild, so a hidden empty component stays "not fully built" forever. The
`DetailRetirementAsyncHism` test passes only because the TEST calls `BuildTreeIfOutdated`
itself in its final phase; production never did.

Fix applied for the second run: when a candidate is not async-building but its tree is out of
date, request `BuildTreeIfOutdated(true,false)` once (counted as `hismRebuilt`) and keep
deferring; eligibility follows on a later probe. No wait, flush or forced GC is introduced.

## Second run with the fix (route captures 10 OFF / 11 ON), same day

Binary: the pilot branch plus the empty-tree rebuild request (`.scratch/retirement-build-2`,
coherent). Fourth bake `temperate-authored-lods-full-4` (identity source changed again),
verified, geometry identical. Automation 8/8 again (`automation-run-2.txt`).

Both arms 18/18 arrivals, 295/297 and 296/297 grounded, zero waiting, 2.2 m/s. Frame medians
identical (23.0 vs 23.1 ms frame, 13.4 ms GPU).

**The ON arm now retires** (`route-capture-11-retirement-telemetry.txt`): final totals
probes=29,017 keys=13 objectsUnrooted=13 hismDeferred=39 hismRebuilt=39 blockedSeconds=0,
meshes 94 resident at the end (new species keep arriving along the route). Every deferral
was a rebuild request and every request later cleared, so the pin from run 1 is gone. Thirteen
retirements on a 344 m out-and-back at the 48 m ring is the honest size of the effect: the
pilot is opportunistic, continuous travel keeps most keys in use, and no byte reclamation is
claimed. What it establishes: the barrier, the alias/root bookkeeping and the rebuild path
work on real assets without touching navigation or frame time. The 256 m unload/revisit case
and actual memory accounting remain to be measured.
