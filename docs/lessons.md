# Lessons learned

Running log of things that cost real debugging time or shaped process.
Newest first. Add an entry whenever a lesson is worth not re-learning.

## 2026-07-29 — the water session: rendering, CA perf, W3 rivers

### The one that cost the most

- **"Avoided the cost" and "the feature does not work" were the same fact.**
  W3 channel carving was built to produce riverbeds and deliberately did NOT
  wire itself into worldgen, avoiding a `kWorldGenVersion` bump, a golden
  re-pin and an HLSL mirror change. That was reported as a clean win and
  accepted as one. It was clean — and it was also exactly why, when river
  discharge was switched on, water landed on unmodified hillside and puddled
  in disconnected patches instead of flowing. `ChannelField` appears nowhere in
  `amplifier.cpp`, `world.h` or `VoxelWorldSubsystem.cpp`.
  **When a change hits its goal by not integrating, check whether integrating
  WAS the goal.** The version bump was not an obstacle to the feature; paying
  it was the feature.

### Verification

- **Conservation proves nothing about correctness.** The SWE breach fixture
  reported `consFail=0`, `sumMinusPoured=+0`, a perfectly balanced four-term
  ledger — while a breach was failing to puncture anything at all and no water
  moved. A ledger proves water was not LOST. It says nothing about whether it
  went anywhere. Every invariant that holds trivially when nothing happens will
  hold loudest exactly when nothing happens.
- **An instrument can carry an alibi for the bug it is meant to catch.** That
  same fixture's verdict text cited `swe.h` §5(a) to explain why zero velocity
  at the breach mouth was CORRECT. The explanation was true in general and
  wrong here, and it would have talked the next reader out of the finding.
  Assertions in a fixture are cheap; prose in a fixture is a claim, and claims
  need the same scrutiny as code.
- **Verify what a number MEANS before reasoning from it.** A whole hypothesis
  (bed re-seating racing puncture detection) was built on `reseated=5383` in a
  log line. That field was printing the worst column's voxel **z coordinate**,
  not a count. `SweGrid::setBed` has exactly one caller in the engine; there is
  no re-seat path to race. The real cause was the opposite — nothing EVER
  re-seats, so a carved bed leaves a permanent wall.

### Measurement

- **Two variables moved, so the measurement was void.** The water CA was first
  measured at ~800 ms/tick, then ~440 ms after optimisation — but the baseline
  ran on pre-v9 terrain and the after-run on v9, with 3.2x the active bricks.
  Re-running the before-case on identical terrain gave the honest number:
  **1996–2154 ms → 402–489 ms, 4.7x**. The first comparison was not "roughly
  half"; it was uninterpretable.
- **A contention guard that cries wolf gets ignored.** The first version of
  `tools/voxel-measure-guard.ps1` reported CONTENDED against seven MSBuild
  processes that had used 0.00 s of CPU in six seconds — idle node-reuse
  workers. It now judges editors on presence (they hold the GPU regardless) and
  build tools on actual CPU burn. A false positive costs more than the false
  negative it was preventing, because it trains you to skip the check.
- **Absolute numbers expire when worldgen moves.** `kWorldGenVersion` went 9 →
  10 mid-session. Every quad count and tick time taken before that describes a
  world that no longer exists. Relative comparisons within a matched pair
  survive; absolutes do not. State the worldgen version beside any water figure.

### Toolchain

- **Three cross-compiler CI failures in one session, one root.** MSVC does not
  warn on missing field initializers; gcc and clang do, and CI runs `-Werror`.
  A green local build proves nothing about CI. Two specific shapes: an
  `operator==` left unused by the very optimisation that deleted its only
  caller, and a struct member (`RiverDiffRecord::course`) that was the ONLY one
  without a default initializer — which made gcc raise at every brace-init that
  stopped short of it, including designated ones. **Give every new struct
  member a default initializer.**

### Process

- **Work left uncommitted while agents are in motion is work at risk.** The
  movement fixes — including the known-floor rule that fixed a reported jump
  bug — sat uncommitted in a worktree for hours across several agent launches
  and merges. They survived, but the play-test build did not have them, so the
  bug was re-reported as still broken. Commit before spawning.
- **Point-in-time notes about LIVE bugs go stale fastest and cost the most.**
  A note describing terrain ring-seam holes as an open bug with a half-built
  T-junction fix was five days out of date: the real cause was a desired-set
  coverage gap, fixed by annulus padding, and the T-junction diagnosis was
  simply wrong. Verify against `docs/status.md` before acting on any note about
  a live defect.

### Architecture recorded while tracing water placement

- **Only two of four water sources are decided by worldgen.** Ocean is a
  constant (sea level IS voxel z=0; `AVoxelOceanActor` holds zero voxel data).
  Cavern lakes are the one real generation decision — `floodZMm` from
  `hash2(seed, ...)`, ~40% dry. CA water is simulation state, persisted
  separately by ADR-0005 precisely because it is irreducible. SWE is promoted
  out of the CA.
- **Surface lakes and ponds are NOT IMPLEMENTED.** Plan §3.7 specifies basin
  detection gated on precip-vs-evaporation plus a hypsometric curve per body;
  none of it exists in `voxel-core`. A dry valley is dry because nothing ever
  fills it — there is no basin-filling pass anywhere. This is the missing
  fourth determinism site for water placement, and the reason inland standing
  water cannot appear no matter what the river layer does.


## 2026-07-19 — M0/M1 kickoff through first verified renders

### Process

- **Verify with pixels, not with builds.** M1 stage 1+2 built clean on two
  engine versions and passed every compile-time check while the terrain was
  100% invisible. Four real bugs (winding, GetUsedMaterials, BindAxisKey
  ensure, silent Python pin-connect failure) were only findable by looking
  at rendered output. The `-VoxelScreenshotAfter` harness exists so every
  future phase produces an image as its acceptance artifact.
- **Isolate variables with switches, not theories.** The invisible-terrain
  debug went: wireframe run (geometry exists?) → `-VoxelDefaultMaterial`
  run (material or mesh?) → log line pointed at the exact missing override.
  Each run answered exactly one question.
- **Cross-toolchain digests catch real bugs early.** The gcc/clang/MSVC
  determinism gate (proxy for the NV/AMD GPU gate) caught a clang-only
  `-Wconversion` narrowing in its first week of existence.
- **Parallel worktree agents work well** with two rules: disjoint file
  ownership per agent, and integration (merges, conflict resolution, final
  gates) centralized in one place.

### UE specifics (see also ue-project source comments)

- Custom `UPrimitiveComponent`s MUST override `GetUsedMaterials` — the
  render-thread verifier silently discards draws using unlisted materials,
  and engine-default materials are exempt, which masks the bug in tests
  that use them.
- `BindAxisKey` accepts only true 1D axes (MouseX/MouseY). Digital keys
  need `UPlayerInput::AddEngineDefinedAxisMapping` + `BindAxis`.
- A bare `KEY=value` token on the UE command line is parsed as the map URL
  (`-log=x.log`, never `LOG=x.log`).
- Headless `MaterialEditingLibrary.connect_*` calls return bool and fail
  silently — check every one.
- UE 5.7→5.8 was a zero-change retarget for our scene proxy — engine
  version drift risk was lower than feared, but retarget before writing
  more renderer code, not after.

### Toolchain

- VS 2026's CMake component can be registered but absent on disk; the
  payload.vsix in the installer cache contains the real binaries. Prefer
  installing the component properly.
- Windows SDK DXC has the SPIR-V options in help but codegen compiled out;
  official GitHub DXC releases include it (pinned in tools/fetch-dxc.ps1).
- PowerShell 5.1 splits unquoted version-like args (`vulkan1.1` → two
  tokens) and mangles non-ASCII in BOM-less scripts — quote args, ASCII
  only in .ps1.

## Design decision index

Architecture doctrine: implementation plan §2 (invariant).
ADRs: docs/adr/ (0001: vendor-neutral GPU compute, cloud-only diffusion).
M1 decisions: docs/m1-plan.md tables (brick size, render chunks, threading,
budgets, edit routing). Worldgen determinism contract: docs/determinism.md.
