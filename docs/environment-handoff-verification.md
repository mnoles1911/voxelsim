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

Validated against commit96286a7 in `D:/voxelsim/Saved/environment-rehearsal-harness-retry.log`:27pages,18visibleResident,3pending,6absent, successful release and normal exit0. The first launch used a mistyped provider and was stopped as an invalid run; it is not pass evidence. A missing-provider invocation then verified refusal before process creation. PowerShell syntax validation passed. This is functional correctness evidence, not a frame-performance or visible handoff acceptance test.
