# voxel-measure-guard.ps1 -- run a measurement, and refuse to believe it if the
# box was shared.
#
# ============================================================================
# WHY THIS EXISTS ALONGSIDE voxel-run-flight-leg.ps1's ONE-EDITOR RULE
# ============================================================================
#
# That rule refuses to START while another editor is alive, and it is right to.
# But it only checks the starting gun. Two things it cannot catch:
#
#   1. A BUILD, not an editor. Another session running Build.bat saturates six
#      physical cores for minutes. It is invisible to an editor-only check, and
#      a CPU-bound measurement taken beside it is simply wrong.
#   2. Contention that ARRIVES MID-RUN. The box can be quiet at second 0 and
#      loaded at second 30. The run completes, writes a plausible log, and the
#      number goes into a measurements file looking exactly like a slow result.
#
# Both failure modes produce the same shape of wrong conclusion the one-editor
# rule was written about: "this configuration made it worse", when what actually
# happened is that something else was on the machine. A crashed run is obviously
# void; a contended run is not, which is what makes it dangerous.
#
# So this samples the whole time the measurement runs, and reports VOID rather
# than a number if anything else showed up. It is deliberately noisy about it:
# the failure mode being defended against is a contaminated number being quietly
# believed, so silence is not an acceptable output.
#
# Usage:
#   .\tools\voxel-measure-guard.ps1 -Exe <path> -Args @(...) -LogPath <file>
#   .\tools\voxel-measure-guard.ps1 -Check          # just report box state
[CmdletBinding()]
param(
    [string]$Exe,
    [string[]]$ExeArgs = @(),
    [string]$LogPath,
    [int]$TimeoutSec = 600,
    [int]$SampleSec = 3,
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

# Processes that make a measurement untrustworthy if they are running.
# UnrealEditor*: another game/editor on the GPU. cl/link/UnrealBuildTool/MSBuild:
# a build saturating the cores. cmake/ninja: a voxel-core build.
$ContendingNames = @(
    'UnrealEditor', 'UnrealEditor-Cmd', 'UnrealBuildTool',
    'cl', 'link', 'MSBuild', 'cmake', 'ninja', 'vxc_tests'
)

function Get-Contenders {
    param([int[]]$ExceptPids = @())
    @(Get-Process -Name $ContendingNames -ErrorAction SilentlyContinue |
        Where-Object { $ExceptPids -notcontains $_.Id })
}

function Format-Contenders {
    param($Procs)
    ($Procs | ForEach-Object { "$($_.Name)(pid $($_.Id))" }) -join ', '
}

if ($Check) {
    $c = Get-Contenders
    if ($c.Count -eq 0) {
        Write-Host 'BOX QUIET -- safe to measure.' -ForegroundColor Green
    } else {
        Write-Host "BOX CONTENDED -- $(Format-Contenders $c)" -ForegroundColor Red
    }
    exit ($(if ($c.Count -eq 0) { 0 } else { 1 }))
}

if (-not $Exe)     { throw 'Missing -Exe' }
if (-not $LogPath) { throw 'Missing -LogPath' }

# --- Pre-check -------------------------------------------------------------
$pre = Get-Contenders
if ($pre.Count -gt 0) {
    throw ("REFUSING TO MEASURE: $($pre.Count) contending process(es) -- $(Format-Contenders $pre). " +
           'Wait for them to finish. A number taken beside these is not a number.')
}

Write-Host "Box quiet. Starting measurement -> $LogPath" -ForegroundColor Cyan
$proc = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -ArgumentList $ExeArgs

# --- Sample for the whole run ---------------------------------------------
# Every intruder seen is recorded with the second it appeared, because "it was
# clean for 50s then a build started" and "it was contended throughout" are
# different stories and only one of them is salvageable by re-running.
$intruders = @{}
$startedAt = Get-Date
$elapsed = 0

while (-not $proc.HasExited -and $elapsed -lt $TimeoutSec) {
    Start-Sleep -Seconds $SampleSec
    $elapsed = [int]((Get-Date) - $startedAt).TotalSeconds
    foreach ($c in (Get-Contenders -ExceptPids @($proc.Id))) {
        $key = "$($c.Name)(pid $($c.Id))"
        if (-not $intruders.ContainsKey($key)) {
            $intruders[$key] = $elapsed
            Write-Host "  !! CONTENTION at ${elapsed}s: $key" -ForegroundColor Red
        }
    }
}

$timedOut = -not $proc.HasExited
if ($timedOut) {
    Write-Host "  Timed out at ${TimeoutSec}s -- killing. Treat as VOID, not as a slow result." -ForegroundColor Yellow
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}

# --- Verdict ---------------------------------------------------------------
Write-Host ''
if ($intruders.Count -gt 0) {
    Write-Host '=============================================================' -ForegroundColor Red
    Write-Host ' VOID -- the box was shared while this ran.' -ForegroundColor Red
    foreach ($k in $intruders.Keys) {
        Write-Host ("   {0}  first seen at {1}s" -f $k, $intruders[$k]) -ForegroundColor Red
    }
    Write-Host ' Do NOT record these numbers. Re-run on a quiet box.' -ForegroundColor Red
    Write-Host '=============================================================' -ForegroundColor Red
    exit 2
}
if ($timedOut) { exit 3 }

Write-Host "CLEAN -- box stayed quiet for the whole ${elapsed}s run. Numbers are usable." -ForegroundColor Green
exit 0
