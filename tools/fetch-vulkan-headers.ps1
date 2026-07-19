# Fetches the official KhronosGroup/Vulkan-Headers source archive (headers
# only — vulkan_core.h etc.) into tools/vulkan-headers/. Required to compile
# voxel-core/bench/gpu_harness.cpp (ADR-0001's Vulkan determinism harness).
#
# No Vulkan SDK dependency: the harness loads the loader (vulkan-1.dll) at
# runtime via LoadLibrary + vkGetInstanceProcAddr, so only the headers (types,
# structs, PFN_ typedefs, constants) are needed at compile time — no import
# lib, no vulkan-1.lib.
#
# Pinned tag — bump deliberately; header changes could shift struct layouts
# the harness relies on.
$ErrorActionPreference = "Stop"
$Tag = "vulkan-sdk-1.4.328.0"
$Url = "https://codeload.github.com/KhronosGroup/Vulkan-Headers/zip/refs/tags/$Tag"
$Dest = Join-Path $PSScriptRoot "vulkan-headers"

if (Test-Path (Join-Path $Dest "include\vulkan\vulkan_core.h")) {
    Write-Host "Vulkan-Headers already present at $Dest"
    exit 0
}
New-Item -ItemType Directory -Force $Dest | Out-Null
$Zip = Join-Path $env:TEMP "vulkan-headers-$Tag.zip"
Write-Host "Downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $Zip

$Extract = Join-Path $env:TEMP "vulkan-headers-extract-$Tag"
if (Test-Path $Extract) { Remove-Item -Recurse -Force $Extract }
Expand-Archive -Path $Zip -DestinationPath $Extract -Force
Remove-Item $Zip

# GitHub source archives nest one "<repo>-<tag>/" directory; flatten it.
$Inner = Join-Path $Extract "Vulkan-Headers-$Tag"
Copy-Item (Join-Path $Inner "*") $Dest -Recurse -Force
Remove-Item -Recurse -Force $Extract

if (-not (Test-Path (Join-Path $Dest "include\vulkan\vulkan_core.h"))) {
    throw "fetch-vulkan-headers: vulkan_core.h not found after extraction"
}
Write-Host "Vulkan-Headers $Tag ready at $Dest"
