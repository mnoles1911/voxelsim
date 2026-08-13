# The whole water pass, in one unattended chain, in the only order that works.
#
# WHY A CHAIN RATHER THAN FOUR COMMANDS: every step here holds the editor
# exclusively, several of them for ten minutes or more, and the ordering
# constraints between them are not obvious from their names. Running them by
# hand means being present for ninety minutes to type the next one, and getting
# the order wrong is silent rather than loud.
#
# THE ORDER, AND WHY EACH EDGE EXISTS
# ===================================
#
#   1. BUILD the VoxelEarth module. It must be first because it cannot run at
#      all while an editor is up -- the running commandlet holds
#      UnrealEditor-VoxelEarth.dll open and the link fails. Everything after
#      this point launches an editor, so this is the only slot it fits in.
#
#   2. REGENERATE M_Underwater. After the build because the C++ that binds it
#      has to exist first? No -- it does not, they are independent artefacts.
#      It is here because if the build fails there is no point spending nine
#      minutes on a material nothing will load.
#
#   3. THE SHORE-FX A/B, full, both arms. This regenerates the water and both
#      terrain materials, so it is what picks up the new water_optics.py
#      constants for the SURFACE. It must come after step 2 only so that the
#      two logs the regen script cross-checks (surface vs underwater optics)
#      are from the same constants -- if the A/B ran first, step 2 would
#      compare against a water log that predated it and warn.
#      It leaves the SHIPPED (ON) arm installed, which step 4 needs.
#
#   4. UNDERWATER CAPTURES. Last, because they need all three of the above: the
#      new C++ (which drives SubmergedDepthM and deletes the height fog), the
#      new material, and the shipped arm on disk.
#
# WHY THE A/B IS BEING RE-SHOT AT ALL, since it already ran once tonight: two
# independent reasons, either of which alone would justify it.
#   * Its "close" pose was wrong -- 1.3 m UNDERWATER rather than a shoreline
#     close-up, because -SpawnAltM measures from the bed and that column is the
#     pond's deep end. Corrected in voxel-shore-fx-ab.ps1 to a bank column read
#     out of the baked shore-distance field.
#   * The water itself changed underneath it. The owner asked for clarity
#     "somewhere in between" the two previous states, so the first run's frames
#     are of water that no longer exists.
# The first run's frames are NOT deleted. Its wide pair is a valid before-shot
# of the clarity change at a pose the new run repeats, and its close pair is an
# accidental but genuine before-shot of the OLD underwater treatment at the
# exact column step 4 photographs.

param(
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [string]$BuildBat = 'D:\UE_5.8\Engine\Build\BatchFiles\Build.bat',
    [switch]$SkipBuild,
    [switch]$SkipAb
)

$ErrorActionPreference = 'Stop'
$Root    = (Resolve-Path "$PSScriptRoot\..").Path
$Project = (Resolve-Path "$Root\ue-project\VoxelEarth.uproject").Path
$Started = Get-Date

function Step([string]$Title, [scriptblock]$Body) {
    $t0 = Get-Date
    Write-Host ""
    Write-Host ("##### $Title") -ForegroundColor Magenta
    & $Body
    Write-Host ("##### $Title done in {0:0} min" -f ((Get-Date) - $t0).TotalMinutes) -ForegroundColor Magenta
}

# One editor per box -- checked once here as well as inside each child script,
# because failing at minute 0 is much better than failing at minute 40.
$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id)" }) -join ', '
    throw "REFUSING TO START: editor already running -- $detail. Wait for it, or stop it."
}

if (-not $SkipBuild) {
    Step 'BUILD VoxelEarthEditor' {
        $log = Join-Path $Root 'Saved\build-underwater.log'
        # Build.bat's own exit code IS meaningful (unlike the editor
        # commandlet's), so it is the test here -- but capture the log too,
        # because a link error naming a const-correctness problem in
        # SubmergedDepthUUAtWorld is the single most likely failure and reading
        # it beats re-running.
        & $BuildBat VoxelEarthEditor Win64 Development -Project="$Project" -WaitMutex *>&1 |
            Tee-Object -FilePath $log | Select-Object -Last 25
        if ($LASTEXITCODE -ne 0) { throw "BUILD FAILED (exit $LASTEXITCODE) -- see $log" }
        # UBT prints this even for a no-op build; its ABSENCE means something
        # went wrong upstream of compilation.
        if (-not (Select-String -Path $log -SimpleMatch 'Result: Succeeded' -ErrorAction SilentlyContinue)) {
            throw "no 'Result: Succeeded' in $log despite exit 0 -- treat the build as UNKNOWN."
        }
    }
}

Step 'REGENERATE M_Underwater' {
    & (Join-Path $PSScriptRoot 'voxel-underwater-regen.ps1') -Editor $Editor
}

if (-not $SkipAb) {
    Step 'SHORE-FX A/B (both arms, both poses, new water)' {
        & (Join-Path $PSScriptRoot 'voxel-shore-fx-ab.ps1') -Editor $Editor
    }
}

Step 'UNDERWATER CAPTURES (three depths)' {
    & (Join-Path $PSScriptRoot 'voxel-underwater-captures.ps1')
}

Write-Host ""
Write-Host ("ALL DONE in {0:0} min" -f ((Get-Date) - $Started).TotalMinutes) -ForegroundColor Green
