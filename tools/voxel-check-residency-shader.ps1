# voxel-check-residency-shader.ps1 -- do the T4-2 residency kernels compile?
#
# WHY THIS EXISTS: voxel-check-worklist-shader.ps1's reason verbatim. The
# editor compiles global shaders at boot, so a half-finished .usf is live input
# to the NEXT agent's leg in a way a half-finished .cpp is not. VoxelResidency-
# Scan.usf now carries six boot-compiled kernels; a syntax error in any one of
# them costs a whole leg to discover. This costs ~2 seconds, no build, no
# editor -- which is the point, because the residency work happens in a lane
# that is not allowed to build.
#
# LIMIT, STATED (same as the sibling scripts): this proves the HLSL compiles
# under DXC with Platform.ush stubbed. It is "the tree is not mid-edit", not a
# substitute for the editor's own compile, and it says NOTHING about whether
# each kernel's FParameters struct declares the loose globals it reads -- that
# is tools/lint-residency-globals.py, and the two are complementary. Run both.
#
# No SPIR-V arm: these kernels are engine-side only (dispatched through RDG,
# never by the Vulkan bench), so DXIL is the one target that exists for them.

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
# A worktree checkout has no fetched dxc; fall back to the main checkout's.
if (-not (Test-Path $Dxc)) { $Dxc = 'D:\voxelsim\tools\dxc\bin\x64\dxc.exe' }
$Usf = Join-Path $Root 'ue-project\Shaders\VoxelResidencyScan.usf'
$Stage = Join-Path $env:TEMP 'voxel-residency-check'

if (-not (Test-Path $Dxc)) { throw "dxc not found at $Dxc (run tools/fetch-dxc.ps1)" }
if (-not (Test-Path $Usf)) { throw "VoxelResidencyScan.usf not found at $Usf" }
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null

# The virtual include path only resolves inside the engine; stage a flat copy
# with the include stubbed. Nothing in the source tree is modified.
(Get-Content $Usf -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub') |
    Out-File (Join-Path $Stage 'VoxelResidencyScan.hlsl') -Encoding utf8

$Src = Join-Path $Stage 'VoxelResidencyScan.hlsl'
$Out = Join-Path $Stage 'out.bin'
$Fail = 0
$Total = 0

function Try-Compile($EntryPoint, $Label) {
    $ArgList = @('-T', 'cs_6_0', '-E', $EntryPoint, '-HV', '2021', '-O3', '-Fo', $script:Out,
                 $script:Src)
    # stderr to a file, not 2>&1: PowerShell 5.1 wraps native stderr lines in
    # NativeCommandError and aborts the run on the first failing kernel.
    $ErrFile = Join-Path $script:Stage 'dxc-stderr.txt'
    $Saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $script:Dxc @ArgList 2>$ErrFile | Out-Null
    $Failed = ($LASTEXITCODE -ne 0)
    $ErrorActionPreference = $Saved
    $Result = if (Test-Path $ErrFile) { Get-Content $ErrFile } else { @() }
    if ($Failed) {
        Write-Host "FAIL  $Label"
        $Result | Select-Object -First 12 | ForEach-Object { Write-Host "      $_" }
        return 1
    }
    Write-Host "ok    $Label"
    return 0
}

# All six entry points. BudgetResolveCS is the -VoxelResidencyAdmitBudget lane
# (2026-08-23); EntryScanCS compiles once but runs in two EmitPhases.
$Total += 1; $Fail += Try-Compile 'FeedbackCS'      'feedback DXIL  FeedbackCS'
$Total += 1; $Fail += Try-Compile 'ZRangeCS'        'zrange   DXIL  ZRangeCS'
$Total += 1; $Fail += Try-Compile 'ExitScanCS'      'exit     DXIL  ExitScanCS'
$Total += 1; $Fail += Try-Compile 'EntryScanCS'     'entry    DXIL  EntryScanCS'
$Total += 1; $Fail += Try-Compile 'BudgetResolveCS' 'budget   DXIL  BudgetResolveCS'
$Total += 1; $Fail += Try-Compile 'AuditCS'         'audit    DXIL  AuditCS'

Write-Host ""
if ($Fail -gt 0) {
    Write-Host "RESIDENCY SHADER CHECK: $Fail of $Total kernels FAILED"
    exit 1
}
Write-Host "RESIDENCY SHADER CHECK: all $Total kernels compile"
exit 0
