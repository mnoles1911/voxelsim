# Regenerate the sky/water material set for ONE arm of the reflected-stars A/B.
#
# WHY A SCRIPT AND NOT THREE PASTED COMMAND LINES
# ===============================================
#
# Two things about this regeneration have already cost this project real time,
# and both are the kind of mistake that reports success:
#
#  1. THE ORDER. create_sky_material.py is the SOLE author of MPC_VoxelSky and it
#     DELETES and recreates the asset every run. Every other sky material binds
#     to that collection, so the order is fixed: sky, then the atmosphere dome,
#     then the water. Running the water first (or alone, after the sky) leaves it
#     bound to a collection that no longer exists in the form it expected. The
#     2026-08-10 failure from getting this wrong was that ALL WATER DREW WITH
#     UE'S DEFAULT MATERIAL while the log said the script had succeeded.
#
#  2. THE ARM. The star-reflection branch is selected by an environment variable
#     read by create_water_voxel_material.py. An environment variable that fails
#     to reach the process does not error -- it silently builds the default arm.
#     A perf A/B whose two halves were the same shader would show a delta of ~0,
#     which is EXACTLY the answer "the feature is free" looks like.
#
# So this script sets the variable, runs the three scripts in the mandated order,
# and then READS BACK from the log the line the generator prints naming the arm
# it actually built. If that line is missing or names the other arm, this fails
# loudly rather than handing back a material nobody has checked.
#
# It also greps for material COMPILE FAILURES. A material that fails to compile
# is silently replaced by the engine default, which renders -- and renders FAST.
# An arm that failed to compile would look like a large perf win.

param(
    [Parameter(Mandatory=$true)][ValidateSet('on','off')][string]$Arm,
    # SWITCH ARMS WITHOUT REBUILDING THE SKY.
    #
    # The ordering rule is "the water material must be authored AFTER
    # MPC_VoxelSky exists in its current form", because create_sky_material.py
    # DELETES and recreates that collection and any material holding a binding to
    # the old one fails to compile. It is NOT a rule that the three scripts must
    # always run as a set.
    #
    # So when the ONLY thing changing between two runs is this script's own arm,
    # re-running the sky is not just slow (about twenty minutes of editor startup
    # on this box), it is the riskier option: it destroys and recreates the
    # collection that the material about to be built binds to, for no reason.
    # Leaving the sky alone means the second arm is authored against the
    # BYTE-IDENTICAL MPC the first arm was, which is what an A/B wants.
    #
    # The ordering rule is still enforced, and not on trust: the water generator
    # re-checks every collection binding by name against the asset as read back,
    # and raises if MPC_VoxelSky is missing or stale. That check is what makes
    # this switch safe rather than a shortcut.
    #
    # Do NOT pass this after changing create_sky_material.py or the dome.
    [switch]$WaterOnly,
    # THE FROZEN-RIPPLE MEASUREMENT ARM (create_water_voxel_material.py's
    # FREEZE_RIPPLE_TIME). Replaces the single Time node driving both ripple
    # systems with a constant, so a burst taken at a frozen pose separates
    # ANIMATION from TEMPORAL INSTABILITY -- without it the two are the same
    # number, and the burst's ~0.43 s shutter spacing amplifies the animation
    # half by about 26x over what the player sees.
    #
    # This is a MEASUREMENT arm. The shipped material is the LIVE one; re-run
    # without this switch to put it back.
    [switch]$FreezeRippleTime,
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [int]$TimeoutSec = 1800
)

$ErrorActionPreference = 'Stop'
$Root    = (Resolve-Path "$PSScriptRoot\..").Path
$Project = (Resolve-Path "$Root\ue-project\VoxelEarth.uproject").Path
$Tools   = Join-Path $Root 'ue-project\Tools'
$LogDir  = Join-Path $Root 'Saved'

# ONE EDITOR PER BOX. Same rule, same reason, as voxel-run-flight-leg.ps1: two
# editors on this machine have previously destroyed each other's frames.
$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id)" }) -join ', '
    throw "REFUSING TO START: editor already running -- $detail."
}

# The value the generator reads. Set for THIS PowerShell process, which
# Start-Process inherits into the child.
$env:VOXEL_WATER_STAR_REFLECT = $(if ($Arm -eq 'on') { '1' } else { '0' })
Write-Host "VOXEL_WATER_STAR_REFLECT=$($env:VOXEL_WATER_STAR_REFLECT)  (arm '$Arm')" -ForegroundColor Cyan
# Set EXPLICITLY in both directions rather than only when asked. An inherited
# leftover from a previous frozen run in the same shell would otherwise build a
# frozen material while the banner said nothing about it.
$env:VOXEL_WATER_FREEZE_TIME = $(if ($FreezeRippleTime) { '1' } else { '0' })
Write-Host "VOXEL_WATER_FREEZE_TIME=$($env:VOXEL_WATER_FREEZE_TIME)  (ripple time $(if ($FreezeRippleTime) { 'FROZEN -- MEASUREMENT ARM' } else { 'LIVE' }))" -ForegroundColor Cyan

# THE MANDATED ORDER. Do not reorder; see the header.
$scripts = if ($WaterOnly) { @('create_water_voxel_material.py') } else { @(
    'create_sky_material.py',
    'create_sky_atmosphere_dome_material.py',
    'create_water_voxel_material.py'
) }
if ($WaterOnly) { Write-Host "  -WaterOnly: sky and dome left untouched (see the switch's note)" -ForegroundColor DarkGray }

$waterLog = $null
foreach ($s in $scripts) {
    $stem = $s -replace '\.py$',''
    $log  = Join-Path $LogDir "regen-$Arm-$stem.log"
    if ($s -like '*water*') { $waterLog = $log }

    Write-Host "=== $s -> $log" -ForegroundColor Gray
    # -AllowCommandletRendering, AND ONLY FOR THE WATER SCRIPT.
    #
    # Without it the commandlet comes up with rhiname="Null", no shader map is
    # ever built, and MaterialEditingLibrary.get_statistics returns every field
    # as 0. That zero is indistinguishable from a genuinely free material, which
    # is precisely the number this A/B is supposed to produce -- so the ONE run
    # that reads instruction counts gets a real RHI. The sky scripts are left on
    # the cheaper path because nothing reads statistics off them.
    $extra = @()
    if ($s -like '*water*') { $extra += '-AllowCommandletRendering' }
    $p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList (@(
        "`"$Project`"", '-run=pythonscript', "-script=`"$Tools\$s`"",
        '-unattended', '-nop4', '-nosplash', "-abslog=`"$log`""
    ) + $extra)
    $p.WaitForExit($TimeoutSec * 1000) | Out-Null
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; throw "$s TIMED OUT after ${TimeoutSec}s" }

    # THE EXIT CODE IS NOT THE TEST, IN EITHER DIRECTION, AND BOTH DIRECTIONS
    # HAVE BITTEN. Measured on this project 2026-08-11: create_sky_material.py
    # ran perfectly and the commandlet still exited 1, because UE's end-of-run
    # "Warning/Error Summary" counts three PRE-EXISTING project errors that have
    # nothing to do with the script (a GeneralProjectSettings ProjectID import
    # and two GameFeatureData asset-manager rules). Gating on the exit code
    # refuses a good regeneration. And the converse is the classic
    # -run=pythonscript trap: an exception inside the script can still leave a 0.
    #
    # The real test is the commandlet's own two markers.
    $ok = Select-String -Path $log -SimpleMatch 'Python script executed successfully' -ErrorAction SilentlyContinue
    $py = @(Select-String -Path $log -SimpleMatch 'LogPython: Error' -ErrorAction SilentlyContinue)
    if (-not $ok -or $py.Count -gt 0) {
        if ($py.Count -gt 0) { $py | Select-Object -First 12 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red } }
        throw "$s FAILED (successMarker=$([bool]$ok), pythonErrors=$($py.Count), exit=$($p.ExitCode)) -- see $log"
    }
}

# --- READ THE ARM BACK OUT OF THE LOG -----------------------------------------
#
# This is the ran-flag. "The script exited 0" is not one: it is equally true of a
# run where the environment variable never arrived.
$armLine = Select-String -Path $waterLog -Pattern 'STAR REFLECTION ARM: (\w+)' -ErrorAction SilentlyContinue |
           Select-Object -Last 1
if (-not $armLine) {
    throw ("no 'STAR REFLECTION ARM' line in $waterLog -- the generator that prints it did not run, " +
           "so which shader is on disk is UNKNOWN. This is a hard failure, not a warning.")
}
$built = $armLine.Matches[0].Groups[1].Value
if ($built -ne $Arm.ToUpper()) {
    throw "ASKED FOR '$($Arm.ToUpper())' BUT THE GENERATOR BUILT '$built' -- $($armLine.Line)"
}
Write-Host "  $($armLine.Line.Substring($armLine.Line.IndexOf('M_WaterVoxel')))" -ForegroundColor Green

# --- AND THE RIPPLE-TIME ARM, ON THE SAME TERMS -------------------------------
$rtLine = Select-String -Path $waterLog -Pattern 'RIPPLE TIME ARM: (\w+)' -ErrorAction SilentlyContinue |
          Select-Object -Last 1
if (-not $rtLine) {
    throw ("no 'RIPPLE TIME ARM' line in $waterLog -- the generator that prints it did not run, so " +
           "whether the water ripple on disk is animated is UNKNOWN. An animation-vs-flicker split " +
           "taken against an unknown arm is not a split.")
}
$rtBuilt = $rtLine.Matches[0].Groups[1].Value
$rtWant = if ($FreezeRippleTime) { 'FROZEN' } else { 'LIVE' }
if ($rtBuilt -ne $rtWant) { throw "ASKED FOR RIPPLE TIME '$rtWant' BUT THE GENERATOR BUILT '$rtBuilt' -- $($rtLine.Line)" }
Write-Host "  $($rtLine.Line.Substring($rtLine.Line.IndexOf('M_WaterVoxel')))" -ForegroundColor Green

# AND THE SAME ARM READ OFF THE PACKAGE ON DISK, NOT OFF THE LOG.
#
# The generator's own read-back goes through the editor Python bindings and on
# this engine build it cannot enumerate expressions -- it prints "Time
# nodes=UNKNOWN" and degrades to reporting its INTENT. Intent is what an
# environment variable that never arrived also reports. So check the artefact:
# a saved UMaterial names every expression class it uses in its package name
# table, so the string "MaterialExpressionTime" is present iff the graph still
# has a live Time node. Zero occurrences is proof of FROZEN; one or more is
# proof of LIVE. This costs a file read and closes the one gap the generator
# cannot close itself.
$uasset = Join-Path $Root 'ue-project\Content\Voxel\M_WaterVoxel.uasset'
if (Test-Path $uasset) {
    $bytes = [System.IO.File]::ReadAllBytes($uasset)
    $text  = [System.Text.Encoding]::ASCII.GetString($bytes)
    $hasTime = $text.Contains('MaterialExpressionTime')
    $diskArm = if ($hasTime) { 'LIVE' } else { 'FROZEN' }
    if ($diskArm -ne $rtWant) {
        throw ("THE PACKAGE ON DISK DISAGREES: asked for ripple time '$rtWant' but " +
               "M_WaterVoxel.uasset reads '$diskArm' (MaterialExpressionTime present=$hasTime). " +
               "The log line above is the generator's intent; this is the asset.")
    }
    Write-Host "  package read-back: M_WaterVoxel.uasset ripple time = $diskArm (MaterialExpressionTime present=$hasTime)" -ForegroundColor Green
}
else { Write-Host "  package read-back SKIPPED: $uasset not found" -ForegroundColor Yellow }

# --- THE DURABLE NUMBER -------------------------------------------------------
$stats = Select-String -Path $waterLog -Pattern 'MATERIAL STATS' -ErrorAction SilentlyContinue | Select-Object -Last 1
if ($stats) { Write-Host "  $($stats.Line.Substring($stats.Line.IndexOf('M_WaterVoxel')))" -ForegroundColor Green }
else        { Write-Host "  material statistics line absent -- instruction counts unavailable" -ForegroundColor Yellow }

# --- COMPILE FAILURES ---------------------------------------------------------
#
# Checked across ALL THREE logs, not just the water one: a sky material that
# failed to compile changes the night the water is measured under.
$failed = @()
foreach ($s in $scripts) {
    $log = Join-Path $LogDir "regen-$Arm-$($s -replace '\.py$','').log"
    $failed += @(Select-String -Path $log -SimpleMatch 'Failed to compile Material' -ErrorAction SilentlyContinue)
}
if ($failed.Count -gt 0) {
    $failed | Select-Object -First 8 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red }
    throw ("$($failed.Count) material compile failures -- a material that fails to compile is replaced by " +
           "the engine DEFAULT material, which renders and is FAST. Any perf arm taken now is meaningless.")
}

Write-Host "ARM '$Arm' BUILT AND VERIFIED" -ForegroundColor Green
