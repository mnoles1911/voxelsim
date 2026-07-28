# Summarise finished flight legs, and REFUSE to summarise unfinished ones.
#
# WHY THIS EXISTS. tools/voxel-run-flight-leg.ps1 stops two legs sharing the box.
# This stops the other half of the same mistake: reading a log while it is still
# being written. A partial leg is not obviously partial -- it has a plausible
# chunks/s, a plausible hole count, and a plausible everything else, because the
# flight profile front-loads the cheap phase. Three times in one session a
# mid-flight log was compared against finished ones:
#
#   * a "confirmation" leg read 508.6 chunks/s against a finished 797.0
#   * a parking leg read 694.4 with 354 holes and a 37% hit rate against a
#     finished 855.3 / 2 holes / 84% -- and nearly became "the revert did not
#     restore parking"
#
# Both look exactly like a real regression. Neither was.
#
# A flight leg emits one "Voxel apply stages" line per -VoxelPerfLogInterval, so
# the window count is the completeness test. -MinWindows defaults to 130, which
# is the 133-134 a 90/120/60 leg at 2s intervals produces, with slack for the
# last window landing after the final log flush.
#
# Ground rule 1 says never conclude from a single run. Its siblings, both learned
# the hard way today: never conclude from a run that was sharing the box, and
# never conclude from a run that had not finished.

param(
    [Parameter(Mandatory=$true)][string[]]$LogName,
    [int]$MinWindows = 130,
    [switch]$AllowPartial
)

$ErrorActionPreference = 'Stop'
$SavedDir = Join-Path (Resolve-Path "$PSScriptRoot\..").Path 'Saved'

$rows = @()
foreach ($name in $LogName) {
    $path = Join-Path $SavedDir "$name.log"
    if (-not (Test-Path $path)) {
        Write-Host "  ${name}: NO LOG" -ForegroundColor Red
        continue
    }

    $lines = Get-Content $path
    $windows = @($lines | Select-String -SimpleMatch 'Voxel apply stages').Count
    if ($windows -lt $MinWindows -and -not $AllowPartial) {
        Write-Host ("  {0}: INCOMPLETE ({1}/{2} windows) -- NOT SUMMARISED. It is still running, or it died. " -f $name, $windows, $MinWindows) -ForegroundColor Yellow
        Write-Host "     A partial flight leg reads like a slow configuration. Wait for it, or pass -AllowPartial and say so in whatever you write." -ForegroundColor Yellow
        continue
    }

    # chunks/s: mean of the per-window figure, which is what the 5s line reports.
    $cps = @($lines | Select-String -Pattern 'chunksPerSec=([0-9.]+)' -AllMatches |
             ForEach-Object { $_.Matches } | ForEach-Object { [double]$_.Groups[1].Value })
    $meanCps = if ($cps.Count) { [math]::Round(($cps | Measure-Object -Average).Average, 1) } else { $null }

    $holes = @($lines | Select-String -Pattern 'holes=([0-9]+)' -AllMatches |
               ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value }) | Select-Object -Last 1

    $poolLine = @($lines | Select-String -SimpleMatch 'Voxel GPU pool:') | Select-Object -Last 1
    $allocFail = if ($poolLine -and $poolLine.Line -match 'allocFail=([0-9]+)') { $Matches[1] } else { 'n/a' }

    $parkLine = @($lines | Select-String -SimpleMatch 'Voxel park (5s window)') | Select-Object -Last 1
    $park = if ($parkLine -and $parkLine.Line -match 'cumulative parked=([0-9]+) adopted=([0-9]+) \(hit ([0-9]+)%\)') {
        "parked=$($Matches[1]) adopted=$($Matches[2]) hit=$($Matches[3])%"
    } else { 'parking off' }

    $rows += [pscustomobject]@{
        Leg = $name; Windows = $windows; ChunksPerSec = $meanCps
        Holes = $holes; AllocFail = $allocFail; Park = $park
    }
}

if ($rows.Count) { $rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host }
