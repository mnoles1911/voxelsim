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
