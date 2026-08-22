# check-march-spike.ps1 -- does VoxelMarchSpike.usf compile, in EVERY permutation?
#
# WHY THIS EXISTS. ue-project/Shaders is shared, and the editor compiles those
# shaders at boot -- so a half-finished .usf is live input to the next leg, in a
# way a half-finished .cpp is not. A boot-time global-shader error costs a whole
# leg to discover.
#
# AND WHY IT CHECKS ALL OF THEM. The error that voided two legs
# ("conditional operator only supports results with numeric scalar, vector, or
# matrix types") fired on PERMUTATION 2 ONLY. Compiling one permutation passes
# it happily. This compiles all eleven: 6 pixel-shader variants (3 skip levels x
# no-fetch on/off), 3 census variants, and the 2 mip reduce kernels.
#
# No editor, no engine, ~2 seconds. It uses tools/dxc, the same compiler the
# determinism gate uses.
#
# LIMIT, STATED: this proves the HLSL compiles. It does not prove UE's own
# preprocessor environment agrees, and it stubs /Engine/Public/Platform.ush
# (VoxelMarchSpike.usf deliberately uses nothing from it). A clean run here is
# "the tree is not mid-edit", which is exactly the question a leg needs answered
# -- it is not a substitute for the editor's own compile.

param(
    # Repo root. Defaults to walking up from this script until ue-project is
    # found, so the script works from tools/, from the repo root, or from a
    # scratch directory -- it does not care where it is kept.
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
$Stage = Join-Path $env:TEMP 'voxel-march-spike-check'

if (-not (Test-Path $Dxc)) { throw "dxc not found at $Dxc" }
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null

# The virtual include paths only resolve inside the engine, so stage flat copies
# with the includes rewritten. Nothing is modified in the source tree.
Copy-Item (Join-Path $Shaders 'VoxelFluidContract.ush') $Stage
(Get-Content (Join-Path $Shaders 'VoxelFluidCollision.ush') -Raw).
    Replace('"/VoxelEarth/VoxelFluidContract.ush"', '"VoxelFluidContract.ush"') |
    Out-File (Join-Path $Stage 'VoxelFluidCollision.ush') -Encoding utf8
(Get-Content (Join-Path $Shaders 'VoxelMarchSpike.usf') -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub').
    Replace('"/VoxelEarth/VoxelFluidCollision.ush"', '"VoxelFluidCollision.ush"') |
    Out-File (Join-Path $Stage 'spike.hlsl') -Encoding utf8

$Src = Join-Path $Stage 'spike.hlsl'
$Out = Join-Path $Stage 'out.dxil'
$Fail = 0

function Try-Compile($Profile, $Entry, $Defines, $Label) {
    $ArgList = @('-T', $Profile, '-E', $Entry, '-HV', '2021', '-Fo', $Out)
    foreach ($d in $Defines) { $ArgList += @('-D', $d) }
    $ArgList += $Src
    # NOT `2>&1` on a native exe: Windows PowerShell 5.1 wraps each stderr line
    # in a NativeCommandError, which with $ErrorActionPreference='Stop' aborts
    # the whole run on the FIRST failing permutation -- so a tree with three
    # broken permutations would report one. stderr goes to a file instead.
    #
    # AND $ErrorActionPreference is dropped to Continue across the call for the
    # same reason: with Stop, 5.1 promotes that NativeCommandError to a
    # terminating error and the script dies on the first bad permutation
    # instead of reporting all eleven. Restored immediately after.
    $ErrFile = Join-Path $script:Stage 'dxc-stderr.txt'
    $Saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $script:Dxc @ArgList 2>$ErrFile | Out-Null
    $Failed = ($LASTEXITCODE -ne 0)
    $ErrorActionPreference = $Saved
    $Result = if (Test-Path $ErrFile) { Get-Content $ErrFile } else { @() }
    if ($Failed) {
        # Write-Host, not Write-Output: in PowerShell a function returns
        # everything on the output stream, so a chatty Write-Output here turns
        # the return value into an array and `$Fail += ...` fails with
        # op_Addition. Messages go to the host; only the tally is returned.
        Write-Host "FAIL  $Label"
        $Result | Select-Object -First 8 | ForEach-Object { Write-Host "      $_" }
        return 1
    }
    Write-Host "ok    $Label"
    return 0
}

foreach ($L in 0, 1, 2) {
    foreach ($NF in 0, 1) {
        $Fail += Try-Compile 'ps_6_0' 'VoxelMarchSpikePS' `
            @("VOXEL_MARCH_SKIP_LEVELS=$L", "VOXEL_FLUID_SOLID_NO_FETCH=$NF") `
            "pixel   skip=$L nofetch=$NF"
    }
}
foreach ($L in 0, 1, 2) {
    $Fail += Try-Compile 'cs_6_0' 'VoxelMarchSpikeCountCS' @("VOXEL_MARCH_SKIP_LEVELS=$L") `
        "census  skip=$L"
}
$Fail += Try-Compile 'cs_6_0' 'VoxelMarchSpikeMipL1CS' @() 'mip     L1 reduce'
$Fail += Try-Compile 'cs_6_0' 'VoxelMarchSpikeMipL2CS' @() 'mip     L2 reduce'

if ($Fail -gt 0) {
    Write-Output ""
    Write-Output "MARCH SPIKE SHADER TREE IS MID-EDIT: $Fail of 11 permutations do not compile."
    Write-Output "Do not start a leg -- it will die at boot on a global-shader error."
    exit 1
}
Write-Output ""
Write-Output "all 11 permutations compile -- the march spike shader tree is safe to boot."
exit 0
