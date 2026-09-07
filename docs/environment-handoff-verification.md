# Real terrain handoff verification

`tools/verify-environment-handoff.ps1` launches one owned DX12 process against an existing terrain/provider cache and asset export. It requires hidden preparation evidence, successful rehearsal and release markers when selected, and normal process exit. Timeout cleanup targets only its own process. It refuses active compilers or unidentified/same-project editors; `-AllowOtherProjectEditors` permits identified separate-project editors after coordination.

Example from the validated site (PowerShell):

```powershell
./tools/verify-environment-handoff.ps1 `
  -TileDir D:/voxelsim/tile-cache/terrain-diffusion-unlabeled-80b9ca451a23eae4/000000000135276f/s1 `
  -FineTileDir D:/voxelsim/tile-cache `
  -FineProviderId terrain-diffusion-unlabeled-80b9ca451a23eae4-b5e821e98 `
  -AssetDir D:/voxelsim/asset-forge/out/engine `
  -Seed 20260719 -SpawnX -39661 -SpawnY -57292 `
  -Mode Rehearse -ExitAfterSeconds 35 -AllowOtherProjectEditors
```

`Prepare` verifies hidden candidate preparation only; `Rehearse` also freezes/drains and revalidates affected pages. Neither publishes ownership. The harness checks the exact fine provider directory before launching, so a mistyped provider cannot silently test a different cache. It does not generate missing terrain.

`-Mode HeldCpu -ExitAfterSeconds 90` exercises bounded private CPU page preparation. It additionally requires `ProductionHeldCpu PASSED` with CPU-only/no-readiness/no-publication flags and the successful private-pack discard/release marker. Use the same existing-cache parameters as the example.

`-Mode HeldGpu -ExitAfterSeconds 90` additionally generates private brick-only GPU
packs and compares canonical decoded materials asynchronously. It requires all
allocated pages to match, successful discard/release and normal process exit.

The next `-Mode VisualPilot -ExitAfterSeconds 90` path launches explicitly with
`-VoxelGpuPoolAlloc=0`; it never switches the live allocator. It requires visual
publication through the isolated diagnostic registry, four nonempty before/after,
steady and Unlit capture files and normal exit. This mode changes rendered ownership only;
it does not validate continuous-frame depth/shadow output, collision, mining,
saves or multiplayer. The latest controlled capture uses the intended green
material in both first-after and settled frames; the harness requires the
intended-material readiness receipt. See the handover for evidence and limits.

Validated against commit96286a7 in `D:/voxelsim/Saved/environment-rehearsal-harness-retry.log`:27pages,18visibleResident,3pending,6absent, successful release and normal exit0. The first launch used a mistyped provider and was stopped as an invalid run; it is not pass evidence. A missing-provider invocation then verified refusal before process creation. PowerShell syntax validation passed. This is functional correctness evidence, not a frame-performance or visible handoff acceptance test.

## Controlled surface-lighting diagnostic

Add `-SurfaceLightingDiagnostic` only with `-Mode VisualPilot` to queue
`voxel.GI.Volume 0`, `voxel.Light.Propagated 0`, then
`voxel.Environment.SurfaceLightingDiagnostic 1` before the promotion command.
The option is off by default; other modes reject it before launching a process.
The harness requires `SurfaceLightingDiagnostic APPLIED enabled=1 volumesOff=1`
in addition to every ordinary publication, capture and exit check.

This is a controlled comparison with both volume-lighting paths disabled. A
successful image does not establish correctness with voxel GI or propagated
lighting enabled, nor does it replace the continuous-frame depth/shadow checks.
The switch changes only this owned diagnostic process; no production default or
live allocator mode is changed.

## Optional diagnostic buffers

Add `-CaptureBuffers` only with VisualPilot to request BaseColor, WorldNormal and
SceneDepthWorldUnits EXR sidecars for before, first-after and steady captures.
The viewport must be960x540. The harness requires exactly nine unique stage/buffer
receipts and nonempty files. Prior buffer console settings are restored before
Unlit capture, on timeout, cancellation and teardown. This path adds diagnostic
render flushing and disk work; do not use it to measure handoff latency.

These are separate sampled frames, not a continuous sequence or shadow proof.
Inspect the numeric depth and material buffers rather than treating file creation
as rendering correctness. The corrected buffer extension passed its real run in
`environment-buffer-association-probe.log`: nine EXRs, normal process exit0.
The first attempt failed because FViewport replaced named screenshot requests;
unique temporary directories and image-write fencing now associate exact outputs.
That earlier first-after BaseColor buffer exposed a gray checker fallback.
The corrected `environment-material-readiness-probe.log` completed normally with
nine EXRs and four views. First-after and settled BaseColor use intended green.
Preparation now waits for matching complete GT/RT shader maps and the intended
render proxy before publication. Continuous-frame and shadow proof remain open.

The follow-up `environment-private-gpu-no-buffer-probe.log` passed normalexit0
without `-CaptureBuffers`. First-after and settled views were inspected and green
with no fallback/glow. This removes the EXR readback overhead from that color
comparison; the visual publication boundary itself remains explicitly blocking.
