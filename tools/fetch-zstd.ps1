# Fetches the official facebook/zstd Windows binary release and installs the
# DLL where VoxelEarth::TryRegisterRuntimeZstd looks for it:
#
#     ue-project/Binaries/ThirdParty/zstd/Win64/libzstd.dll
#
# WHY THIS EXISTS AT ALL, i.e. why the DLL is not committed. `.vxtl` v2
# CODEC_ZSTD fine tiles compress ~6x (measured: 201.4 MB -> 33.4 MB on tile
# (-5,2)), and without a zstd they are refused whole with
# FineError::kNoDecompressor. voxel-core deliberately links none of its own --
# ThirdParty/Blosc's libblosc.lib already statically links a zstd into the same
# binary, and a second copy of those C symbols at a version nobody chose has
# wrong terrain as its failure mode rather than a link error. So the decoder is
# INJECTED, bound through the platform's dynamic loader at startup, and nothing
# zstd ends up in our link at all.
#
# LICENCE, because this ships in a commercial game. zstd is dual-licensed
# BSD-3-Clause OR GPLv2, at the user's option (LICENSE and COPYING in the
# release). Take the BSD branch: permissive, no copyleft, no source-disclosure
# obligation. The obligations that remain are small and are handled by this
# script writing LICENSE.txt next to the DLL --
#   * reproduce the copyright notice and the BSD text in your third-party
#     attributions (ship LICENSE.txt, or paste it into your credits screen);
#   * do not use Meta's or its contributors' names to endorse the product.
# Nothing here requires publishing your own source.
#
# WHY THE OFFICIAL RELEASE RATHER THAN WHATEVER IS ON THE BOX. The DLL used
# during bring-up was Git for Windows' copy -- MinGW-built, provenance "whatever
# Git shipped". That is fine to prove a code path and not fine to ship: nobody
# can say which zstd is in the product or answer a licence question about it.
# A pinned official release makes both answerable.
#
# BUILDING FROM SOURCE IS THE OTHER DEFENSIBLE CHOICE and this script does not
# stop you: point -Dest at your own build output. Prefer it if you need a
# specific toolchain, a decompress-only build, or your own code-signing. The
# binding is by NAME at runtime (ZSTD_decompress / ZSTD_isError), so any
# conformant zstd works and swapping it needs no rebuild of anything. Frame
# format has been stable since 1.0 and decode is bit-exact by specification, so
# the choice cannot change terrain.
param(
    # Pinned deliberately. Bump it on purpose, not incidentally: this is a
    # shipping dependency and "whatever was latest that day" is not a provenance
    # answer. Decode is bit-exact across versions, so a bump is a supply-chain
    # decision, never a terrain one.
    [string]$Version = "1.5.6",
    [string]$Dest = (Join-Path (Split-Path $PSScriptRoot -Parent) "ue-project\Binaries\ThirdParty\zstd\Win64")
)

$ErrorActionPreference = "Stop"

$Dll = Join-Path $Dest "libzstd.dll"
if (Test-Path $Dll) {
    Write-Host "zstd already present at $Dll"
    Write-Host "  (delete it and re-run to change version)"
    exit 0
}

$Name = "zstd-v$Version-win64"
$Url = "https://github.com/facebook/zstd/releases/download/v$Version/$Name.zip"
$Zip = Join-Path $env:TEMP "$Name.zip"
$Extract = Join-Path $env:TEMP "$Name-extract"

Write-Host "Downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $Zip
if (Test-Path $Extract) { Remove-Item -Recurse -Force $Extract }
Expand-Archive -Path $Zip -DestinationPath $Extract -Force
Remove-Item $Zip

$Found = Get-ChildItem -Path $Extract -Recurse -Filter "*.dll" |
    Where-Object { $_.Name -match "^(lib)?zstd\.dll$" } | Select-Object -First 1
if (-not $Found) { throw "no libzstd.dll / zstd.dll inside $Url" }

New-Item -ItemType Directory -Force $Dest | Out-Null
Copy-Item $Found.FullName $Dll -Force

# Ship the licence beside the binary. This is what a third-party attribution
# page is assembled from, and keeping it adjacent to the DLL is what stops the
# binary outliving the record of what it is.
#
# FETCHED FROM THE TAGGED SOURCE, NOT TAKEN FROM THE ZIP, because the official
# win64 release archive does NOT contain one -- checked, on v1.5.6: the zip is
# binaries only. A build that silently shipped no licence text would satisfy the
# build and fail the legal review, so this is a hard error rather than a
# best-effort copy.
foreach ($L in @("LICENSE", "COPYING")) {
    $Raw = "https://raw.githubusercontent.com/facebook/zstd/v$Version/$L"
    $Out = Join-Path $Dest "$L.txt"
    Write-Host "Fetching $Raw"
    Invoke-WebRequest -Uri $Raw -OutFile $Out
    if (-not (Test-Path $Out) -or (Get-Item $Out).Length -eq 0) {
        throw "could not fetch $L for zstd v$Version -- refusing to install a binary with no licence text beside it"
    }
}
Remove-Item -Recurse -Force $Extract

$Hash = (Get-FileHash $Dll -Algorithm SHA256).Hash
Write-Host ""
Write-Host "Installed zstd v$Version -> $Dll" -ForegroundColor Green
Write-Host "  sha256 $Hash"
Write-Host "  Record that hash with the version in your third-party manifest."
Write-Host ""
Write-Host "Verify it is actually bound: launch and grep the log for"
Write-Host "  'CODEC_ZSTD: decompressor bound at RUNTIME from'"
Write-Host "A CODEC_RAW-only build will say 'NO decompressor registered' instead,"
Write-Host "and that is a warning, not an error -- RAW tiles are unaffected."
