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

# Two classes, because presence and contention are not the same thing.
#
# PRESENCE IS ENOUGH for an editor: it holds the GPU, VRAM and the project's
# Saved/ directory even when it is sitting at an idle frame, and two editors is
# the exact scenario the one-editor rule exists for.
$ContendByPresence = @('UnrealEditor', 'UnrealEditor-Cmd')

# PRESENCE IS NOT ENOUGH for build tools. MSBuild keeps worker nodes alive for
# reuse long after a build finishes -- this guard's first version reported
# "BOX CONTENDED -- MSBuild x7" against seven processes that had used 0.00s of
# CPU in the previous six seconds. A guard that cries wolf gets ignored, which
# would cost more than the false negative it was protecting against. So these
# are judged on whether they are actually BURNING CPU.
$ContendByCpu = @('UnrealBuildTool', 'cl', 'link', 'MSBuild', 'cmake', 'ninja', 'vxc_tests')

# CPU-seconds consumed within the sample window before a build tool counts.
# Deliberately low: a real compile pins a core, so anything genuinely working
# clears this by an order of magnitude, and idle nodes sit at exactly 0.
$CpuBusyThresholdSec = 0.25

function Get-Contenders {
    param([int[]]$ExceptPids = @(), [double]$WindowSec = 2.0)

    $found = @()
    $found += @(Get-Process -Name $ContendByPresence -ErrorAction SilentlyContinue |
        Where-Object { $ExceptPids -notcontains $_.Id })

    $before = @{}
    foreach ($p in @(Get-Process -Name $ContendByCpu -ErrorAction SilentlyContinue |
                     Where-Object { $ExceptPids -notcontains $_.Id })) {
        $before[$p.Id] = $p.CPU
    }
    if ($before.Count -gt 0) {
        Start-Sleep -Milliseconds ([int]($WindowSec * 1000))
        foreach ($p in @(Get-Process -Name $ContendByCpu -ErrorAction SilentlyContinue |
                         Where-Object { $ExceptPids -notcontains $_.Id })) {
            # A process that appeared DURING the window counts: it is new work,
            # and we cannot prove it was idle.
            $delta = if ($before.ContainsKey($p.Id)) { $p.CPU - $before[$p.Id] } else { [double]::MaxValue }
            if ($delta -ge $CpuBusyThresholdSec) { $found += $p }
        }
    }
    @($found)
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
