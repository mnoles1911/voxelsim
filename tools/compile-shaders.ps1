# Compiles every voxel-core worldgen kernel to BOTH ADR-0001 targets:
#   DXIL   (UE / D3D12)          -> build/shaders/<name>.<entry>.dxil
#   SPIR-V (Vulkan harness)      -> build/shaders/<name>.<entry>.spv
# Fails loudly if either target fails — one source, two backends, always.
# Run tools/fetch-dxc.ps1 once first.
#
# -UpdatePrebuilt ALSO copies the .spv into voxel-core/shaders/prebuilt/, which
# is the directory vxc_gpu actually loads (VXC_SPV_DIR in bench/CMakeLists.txt).
#
# WHY THAT SWITCH EXISTS. This script used to write only build/shaders, while
# worldgen.ush's version-skew #error told you to "respin
# voxel-core/shaders/prebuilt via tools/compile-shaders.ps1" -- something it did
# not do. So editing the shader, running this, and running vxc_gpu silently
# tested MONTHS-OLD bytecode against fresh CPU code. Without -UpdatePrebuilt the
# script now at least says so out loud.
param([switch]$UpdatePrebuilt)

$ErrorActionPreference = "Stop"
$Dxc = Join-Path $PSScriptRoot "dxc\bin\x64\dxc.exe"
if (-not (Test-Path $Dxc)) { throw "DXC not found - run tools/fetch-dxc.ps1 first" }

$Root = Split-Path $PSScriptRoot -Parent
$OutDir = Join-Path $Root "build\shaders"
New-Item -ItemType Directory -Force $OutDir | Out-Null

# kernel source -> entry points
$Kernels = @{ "voxel-core\shaders\worldgen.ush" = @("ColumnMain", "VoxelizeMain", "MeshCountMain", "MeshEmitMain", "ScanBlocksMain", "ScanSumsMain", "ScanAddMain") }

foreach ($Src in $Kernels.Keys) {
    $SrcPath = Join-Path $Root $Src
    $Base = [IO.Path]::GetFileNameWithoutExtension($SrcPath)
    foreach ($Entry in $Kernels[$Src]) {
        $Dxil = Join-Path $OutDir "$Base.$Entry.dxil"
        $Spv = Join-Path $OutDir "$Base.$Entry.spv"
        Write-Host "[$Base/$Entry] DXIL"
        & $Dxc -T cs_6_0 -E $Entry -O3 $SrcPath -Fo $Dxil
        if ($LASTEXITCODE -ne 0) { throw "DXIL compile failed: $Src/$Entry" }
        Write-Host "[$Base/$Entry] SPIR-V"
        # HLSL register classes (b/t/u/s) are separate namespaces but Vulkan
        # descriptor bindings are one flat space per set; without shifts, b0/
        # t0/u0 would all collide at (set=0, binding=0). Shift t- and u-type
        # registers so every resource in worldgen.ush lands on a distinct
        # binding: b0->0, t0->1, t1->2, u0->3, t3->4, u2->5 (see
        # voxel-core/bench/gpu_harness.cpp, which hardcodes these same binding
        # numbers for both the ColumnMain and VoxelizeMain descriptor sets).
        & $Dxc -T cs_6_0 -E $Entry -O3 -spirv "-fspv-target-env=vulkan1.1" `
            -fvk-b-shift 0 0 -fvk-t-shift 1 0 -fvk-u-shift 3 0 $SrcPath -Fo $Spv
        if ($LASTEXITCODE -ne 0) { throw "SPIR-V compile failed: $Src/$Entry" }
    }
}
Write-Host "All kernels compiled to $OutDir"

# The directory vxc_gpu actually loads. Keep this name in step with
# VXC_SPV_DIR in voxel-core/bench/CMakeLists.txt.
$Prebuilt = Join-Path $Root "voxel-core\shaders\prebuilt"
$Stale = @()
foreach ($Spv in Get-ChildItem (Join-Path $OutDir "*.spv")) {
    $Target = Join-Path $Prebuilt $Spv.Name
    if (-not (Test-Path $Target)) { $Stale += $Spv.Name; continue }
    $a = Get-FileHash $Spv.FullName -Algorithm SHA256
    $b = Get-FileHash $Target -Algorithm SHA256
    if ($a.Hash -ne $b.Hash) { $Stale += $Spv.Name }
}

if ($UpdatePrebuilt) {
    Copy-Item (Join-Path $OutDir "*.spv") $Prebuilt -Force
    if ($Stale.Count -gt 0) {
        Write-Host "Updated prebuilt SPIR-V ($($Stale.Count) changed): $($Stale -join ', ')" -ForegroundColor Cyan
        Write-Host "These are COMMITTED artifacts - include them in the commit." -ForegroundColor Cyan
    } else {
        Write-Host "prebuilt SPIR-V already up to date." -ForegroundColor Green
    }
} elseif ($Stale.Count -gt 0) {
    Write-Warning ("prebuilt SPIR-V is STALE for: " + ($Stale -join ", "))
    Write-Warning "vxc_gpu loads voxel-core/shaders/prebuilt, NOT build/shaders, so it will"
    Write-Warning "test the OLD bytecode against your new CPU code and the result is meaningless."
    Write-Warning "Re-run with -UpdatePrebuilt."
} else {
    Write-Host "prebuilt SPIR-V matches this build." -ForegroundColor Green
}
