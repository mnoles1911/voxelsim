# voxel-check-gimarch-shader.ps1 -- does VoxelGIMarch.usf compile, in every
# permutation?
#
# THE SAME REASON tools\voxel-check-shadowmarch-shader.ps1 EXISTS, and the same
# rule: ue-project\Shaders is shared, the editor compiles global shaders AT
# BOOT, and a half-finished .usf there does not fail a build -- it fails an
# editor launch and costs a whole leg. Run this before handing the tree to a
# build slot.
#
# Like the shadow march, VoxelGIMarch.usf has NO ENGINE INCLUDES beyond
# Platform.ush, so what dxc compiles here is byte-for-byte the code UE's
# compiler will see, minus only UE's preprocessor environment.
#
#     walk 0  flat        VoxelMarchTraverseBrick            -- THE DEFAULT
#     walk 1  hier        VoxelMarchTraverseBrickHier
#     walk 2  compare     both, per-direction disagreement counters
#     source 0 guard      must FAIL on the #error in VoxelGIMarch.usf -- a GI
#                         cone fired from anywhere in the 70 m light field
#                         cannot be contained by the fluid occupancy volume's
#                         51.2 m box, and the script fails if that refusal ever
#                         stops firing.
#
# No editor, no engine, ~2 seconds. Uses tools\dxc, the same compiler the
# determinism gate uses.

param(
    [string]$Root
)

$ErrorActionPreference = 'Stop'
if (-not $Root) {
    $Probe = $PSScriptRoot
    while ($Probe -and -not (Test-Path (Join-Path $Probe 'ue-project'))) {
        $Parent = Split-Path $Probe -Parent
        if ($Parent -eq $Probe) { break }
        $Probe = $Parent
    }
    if ($Probe -and (Test-Path (Join-Path $Probe 'ue-project'))) { $Root = $Probe }
    else { $Root = 'D:\voxelsim' }
}
$Root = (Resolve-Path $Root).Path
$Dxc = Join-Path $Root 'tools\dxc\bin\x64\dxc.exe'
$Shaders = Join-Path $Root 'ue-project\Shaders'
$Stage = Join-Path $env:TEMP 'voxel-gimarch-check'

if (-not (Test-Path $Dxc)) { throw "dxc not found at $Dxc" }
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null

# The virtual include paths only resolve inside the engine, so stage flat copies
# with the includes rewritten. NOTHING IN THE SOURCE TREE IS MODIFIED.
Copy-Item (Join-Path $Shaders 'VoxelFluidContract.ush') $Stage
Copy-Item (Join-Path $Shaders 'VoxelMaterialPalette.ush') $Stage
(Get-Content (Join-Path $Shaders 'VoxelFluidCollision.ush') -Raw).
    Replace('"/VoxelEarth/VoxelFluidContract.ush"', '"VoxelFluidContract.ush"') |
    Out-File (Join-Path $Stage 'VoxelFluidCollision.ush') -Encoding utf8
(Get-Content (Join-Path $Shaders 'VoxelBrickTraverse.ush') -Raw).
    Replace('"/VoxelEarth/VoxelFluidCollision.ush"', '"VoxelFluidCollision.ush"').
    Replace('"/VoxelEarth/VoxelMaterialPalette.ush"', '"VoxelMaterialPalette.ush"') |
    Out-File (Join-Path $Stage 'VoxelBrickTraverse.ush') -Encoding utf8
(Get-Content (Join-Path $Shaders 'VoxelGIMarch.usf') -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub (same as voxel-check-shadowmarch-shader.ps1)').
    Replace('"/VoxelEarth/VoxelBrickTraverse.ush"', '"VoxelBrickTraverse.ush"') |
    Out-File (Join-Path $Stage 'gimarch.hlsl') -Encoding utf8

$Src = Join-Path $Stage 'gimarch.hlsl'
$Out = Join-Path $Stage 'out.dxil'
$Fail = 0

function Try-Compile($Profile, $Entry, $Defines, $Label, $ExpectFailure = $false) {
    $ArgList = @('-T', $Profile, '-E', $Entry, '-HV', '2021', '-Fo', $Out)
    foreach ($d in $Defines) { $ArgList += @('-D', $d) }
    $ArgList += $Src
    # NOT `2>&1` on a native exe: Windows PowerShell 5.1 wraps each stderr line
    # in a NativeCommandError, which with $ErrorActionPreference='Stop' aborts
    # on the FIRST failing permutation. stderr goes to a file instead.
    $ErrFile = Join-Path $script:Stage 'dxc-stderr.txt'
    $Saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $script:Dxc @ArgList 2>$ErrFile | Out-Null
    $Failed = ($LASTEXITCODE -ne 0)
    $ErrorActionPreference = $Saved
    $Result = if (Test-Path $ErrFile) { Get-Content $ErrFile } else { @() }

    if ($ExpectFailure) {
        if ($Failed) { Write-Host "ok    $Label (refused, as designed)"; return 0 }
        Write-Host "FAIL  $Label -- this MUST NOT compile. The source guard in"
        Write-Host "      VoxelGIMarch.usf is gone, so a source-0 GI permutation can now exist by"
        Write-Host "      accident and march a volume that cannot contain its rays."
        return 1
    }
    if ($Failed) {
        Write-Host "FAIL  $Label"
        $Result | Select-Object -First 10 | ForEach-Object { Write-Host "      $_" }
        return 1
    }
    Write-Host "ok    $Label"
    return 0
}

# Walk 0 is the shipped default and is compiled with SKIP_LEVELS 0, which is the
# level-0 flat control every existing leg measured. Walks 1 and 2 need
# SKIP_LEVELS 2 because that is what selects the two-level (brick mask + chunk
# mask) hierarchy inside VoxelMarchTraverseBrickHier -- with it at 0 the "hier"
# arm would silently be a second flat walk, and the comparison arm would compare
# a walk against itself and report a clean zero forever.
$Fail += Try-Compile 'cs_6_0' 'VoxelGIMarchMain' `
    @('VOXEL_MARCH_SOURCE=1', 'GIMARCH_WALK=0', 'GIMARCH_TILE=4', 'VOXEL_MARCH_SKIP_LEVELS=0') 'gi march  walk=0 flat (default)'
$Fail += Try-Compile 'cs_6_0' 'VoxelGIMarchMain' `
    @('VOXEL_MARCH_SOURCE=1', 'GIMARCH_WALK=1', 'GIMARCH_TILE=4', 'VOXEL_MARCH_SKIP_LEVELS=2') 'gi march  walk=1 hier'
$Fail += Try-Compile 'cs_6_0' 'VoxelGIMarchMain' `
    @('VOXEL_MARCH_SOURCE=1', 'GIMARCH_WALK=2', 'GIMARCH_TILE=4', 'VOXEL_MARCH_SKIP_LEVELS=2') 'gi march  walk=2 compare'
$Fail += Try-Compile 'cs_6_0' 'VoxelGIMarchMain' `
    @('VOXEL_MARCH_SOURCE=0', 'GIMARCH_WALK=0', 'GIMARCH_TILE=4', 'VOXEL_MARCH_SKIP_LEVELS=0') 'source-0 guard' $true

# ---------------------------------------------------------------------------
# THE PERMUTATIONS MUST ACTUALLY DIFFER, AND THAT IS A SEPARATE QUESTION FROM
# WHETHER THEY COMPILE.
# ---------------------------------------------------------------------------
#
# "It compiled" does not mean "the arm exists". If GIMARCH_WALK stopped
# selecting anything -- a typo in the #if, a define that never reaches the
# preprocessor, a walk 2 whose second call got folded away because the compiler
# proved it dead -- every permutation above still reports ok, and walk 2 would
# then report ZERO DISAGREEMENTS FOREVER while measuring one walk against
# itself. A clean zero from an arm that never ran is this project's single most
# repeated failure, and the attribution arm is the last place it can be
# afforded: its whole job is to tell an inherited leak from one of ours.
#
# So the binaries are compared. walk 0 (flat) and walk 1 (hier) must differ from
# each other, and walk 2 must differ from BOTH -- it contains both walks, so it
# is substantially larger than either. Sizes are not asserted against fixed
# numbers (that would fail on every compiler update); the DISTINCTNESS is, and
# distinctness is the property that matters.
Write-Host ''
$Hashes = @{}
foreach ($w in 0, 1, 2) {
    $skip = if ($w -eq 0) { 0 } else { 2 }
    $ArgList = @('-T', 'cs_6_0', '-E', 'VoxelGIMarchMain', '-HV', '2021',
                 '-Fo', (Join-Path $Stage "walk$w.dxil"),
                 '-D', 'VOXEL_MARCH_SOURCE=1', '-D', "GIMARCH_WALK=$w",
                 '-D', 'GIMARCH_TILE=4', '-D', "VOXEL_MARCH_SKIP_LEVELS=$skip", $Src)
    $Saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $Dxc @ArgList 2>(Join-Path $Stage 'dxc-stderr.txt') | Out-Null
    $ErrorActionPreference = $Saved
    $f = Join-Path $Stage "walk$w.dxil"
    $Hashes[$w] = @{ Hash = (Get-FileHash $f -Algorithm SHA256).Hash; Size = (Get-Item $f).Length }
    Write-Host ("      walk=$w skip=$skip  dxil bytes={0}" -f $Hashes[$w].Size)
}
if ($Hashes[0].Hash -eq $Hashes[1].Hash) {
    Write-Host 'FAIL  walk=0 and walk=1 produced BYTE-IDENTICAL DXIL.'
    Write-Host '      GIMARCH_WALK is not selecting a walk. The "hier" arm is a second copy of'
    Write-Host '      the flat one, and any comparison against it is a walk compared with itself.'
    $Fail += 1
} elseif ($Hashes[2].Hash -eq $Hashes[0].Hash -or $Hashes[2].Hash -eq $Hashes[1].Hash) {
    Write-Host 'FAIL  walk=2 is byte-identical to one of the single-walk permutations.'
    Write-Host '      The comparison arm contains only ONE walk, so its per-direction'
    Write-Host '      disagreement counters can only ever read zero. That reads as "the'
    Write-Host '      hierarchy agrees", which is exactly the wrong conclusion to hand back.'
    $Fail += 1
} elseif ($Hashes[2].Size -le [Math]::Max($Hashes[0].Size, $Hashes[1].Size)) {
    Write-Host 'FAIL  walk=2 is no larger than the bigger single-walk permutation.'
    Write-Host '      It should carry BOTH walks. A compare arm smaller than the sum is a'
    Write-Host '      compare arm with something optimised out of it.'
    $Fail += 1
} else {
    Write-Host 'ok    all three walks produce DISTINCT binaries; walk=2 carries both'
}

if ($Fail -gt 0) {
    Write-Host ''
    Write-Host "$Fail permutation(s) failed. The shader tree is MID-EDIT: do not boot an"
    Write-Host 'editor or hand this tree to a build slot until this script passes.'
    exit 1
}
Write-Host ''
Write-Host 'All GI-march permutations compile. The tree is not mid-edit (this proves'
Write-Host "nothing about UE's own preprocessor environment -- the first real compile is"
Write-Host 'the first editor boot after the paired build).'
exit 0
