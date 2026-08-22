# Record that the tree was built successfully, for voxel-run-gpu-arm.ps1's
# shader-vs-binary guard.
#
# WHY A STAMP AND NOT THE DLL'S MTIME. A shader-only change leaves the linker
# nothing to do, so the DLL keeps its old timestamp and the guard refuses every
# leg forever -- correct in form, useless in practice. This records "the tree
# was last known good at time T", which is what the guard means. Run it ONLY
# after a build that reported success; stamping a failed build re-arms exactly
# the trap the guard exists to prevent.
$ErrorActionPreference = 'Stop'
$stamp = Join-Path (Resolve-Path "$PSScriptRoot\..").Path 'Saved\.shader-build-stamp'
Set-Content -Path $stamp -Value (Get-Date -Format 'o') -Encoding utf8
Write-Host "  build stamped: $(Get-Date -Format 'HH:mm:ss')" -ForegroundColor Green
