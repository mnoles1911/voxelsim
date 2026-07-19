# Compiles every voxel-core worldgen kernel to BOTH ADR-0001 targets:
#   DXIL   (UE / D3D12)          -> build/shaders/<name>.<entry>.dxil
#   SPIR-V (Vulkan harness)      -> build/shaders/<name>.<entry>.spv
# Fails loudly if either target fails — one source, two backends, always.
# Run tools/fetch-dxc.ps1 once first.
$ErrorActionPreference = "Stop"
$Dxc = Join-Path $PSScriptRoot "dxc\bin\x64\dxc.exe"
if (-not (Test-Path $Dxc)) { throw "DXC not found - run tools/fetch-dxc.ps1 first" }

$Root = Split-Path $PSScriptRoot -Parent
$OutDir = Join-Path $Root "build\shaders"
New-Item -ItemType Directory -Force $OutDir | Out-Null

# kernel source -> entry points
$Kernels = @{ "voxel-core\shaders\worldgen.hlsl" = @("ColumnMain") }

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
        & $Dxc -T cs_6_0 -E $Entry -O3 -spirv "-fspv-target-env=vulkan1.1" $SrcPath -Fo $Spv
        if ($LASTEXITCODE -ne 0) { throw "SPIR-V compile failed: $Src/$Entry" }
    }
}
Write-Host "All kernels compiled to $OutDir"
