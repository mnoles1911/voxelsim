# Lessons learned

Running log of things that cost real debugging time or shaped process.
Newest first. Add an entry whenever a lesson is worth not re-learning.

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
