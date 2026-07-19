# Fetches the official Microsoft DXC release (includes the SPIR-V backend,
# which the Windows SDK build compiles out) into tools/dxc/. Required for
# compiling voxel-core/shaders/*.hlsl to SPIR-V for the Vulkan determinism
# harness (ADR-0001). Pinned version — bump deliberately, shader binaries
# are part of the determinism surface.
$ErrorActionPreference = "Stop"
$Version = "v1.9.2602.24"
$Asset = "dxc_2026_05_27.zip"
$Url = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/$Version/$Asset"
$Dest = Join-Path $PSScriptRoot "dxc"

if (Test-Path (Join-Path $Dest "bin\x64\dxc.exe")) {
    Write-Host "DXC already present at $Dest"
    exit 0
}
New-Item -ItemType Directory -Force $Dest | Out-Null
$Zip = Join-Path $env:TEMP $Asset
Write-Host "Downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $Zip
Expand-Archive -Path $Zip -DestinationPath $Dest -Force
Remove-Item $Zip
& (Join-Path $Dest "bin\x64\dxc.exe") --version
