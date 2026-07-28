# Prove that a set of legs did not share the box, from the logs themselves.
#
# WHY THIS EXISTS SEPARATELY FROM THE GUARD IN voxel-run-flight-leg.ps1. That
# guard refuses to start while another editor is alive, which PREVENTS the
# problem. This DETECTS it after the fact, and the two are not the same thing:
#
#   * a leg launched by hand, or by an older script, never saw the guard
#   * a leg launched before the guard existed is still sitting in Saved/
#   * the guard checks process liveness at START; it cannot speak for a run that
#     was already finishing, or for anything launched by another tool
#
# On 2026-07-27 two legs ran concurrently for 86 seconds and the contaminated
# result went into a measurements file as a REJECTED configuration with a
# confident mechanical explanation attached. It was caught by a human noticing
# two game windows on screen. This script is what that human was doing, made
# repeatable: a contended leg looks exactly like a slow configuration, and
# nothing in the log says otherwise.
#
# Every UE log line carries [YYYY.MM.DD-HH.MM.SS], so first-line and last-line
# timestamps bound each run. If leg N+1 starts before leg N ends, they overlap
# and BOTH are void -- not just the second one, because contention is mutual.
#
# Usage:
#   tools\voxel-audit-leg-overlap.ps1 -LogName t41s-lead0-1,t41s-lead1-1
#   tools\voxel-audit-leg-overlap.ps1 -Pattern 't41*'

param(
    [string[]]$LogName,
    [string]$Pattern
)

$ErrorActionPreference = 'Stop'
$SavedDir = Join-Path (Resolve-Path "$PSScriptRoot\..").Path 'Saved'

if (-not $LogName -and -not $Pattern) {
    throw 'Pass -LogName or -Pattern.'
}
$files = if ($Pattern) {
    Get-ChildItem (Join-Path $SavedDir "$Pattern.log")
} else {
    $LogName | ForEach-Object { Get-Item (Join-Path $SavedDir "$_.log") }
}

$rx = [regex]'\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2})'
$runs = @()
foreach ($f in $files) {
    $lines = Get-Content $f.FullName
    $stamps = @($lines | ForEach-Object { $m = $rx.Match($_); if ($m.Success) { $m.Groups[1].Value } })
    if ($stamps.Count -lt 2) {
        Write-Host "  $($f.BaseName): no usable timestamps -- SKIPPED" -ForegroundColor Yellow
        continue
    }
    $fmt = 'yyyy.MM.dd-HH.mm.ss'
    $runs += [pscustomobject]@{
        Leg   = $f.BaseName
        Start = [datetime]::ParseExact($stamps[0], $fmt, $null)
        End   = [datetime]::ParseExact($stamps[-1], $fmt, $null)
    }
}

$runs = @($runs | Sort-Object Start)
$overlaps = @()
for ($i = 1; $i -lt $runs.Count; $i++) {
    # Compare against the max End so far, not just the previous leg: a long leg
    # can span several shorter ones, and comparing only to its immediate
    # predecessor would miss that.
    $maxEnd = ($runs[0..($i-1)] | Measure-Object -Property End -Maximum).Maximum
    if ($runs[$i].Start -lt $maxEnd) {
        $overlaps += [pscustomobject]@{
            Leg     = $runs[$i].Leg
            Started = $runs[$i].Start
            While   = ($runs[0..($i-1)] | Where-Object { $_.End -gt $runs[$i].Start }).Leg -join ', '
            Seconds = [int]($maxEnd - $runs[$i].Start).TotalSeconds
        }
    }
}

$runs | Select-Object Leg, @{n='Start';e={$_.Start.ToString('HH:mm:ss')}},
                            @{n='End';e={$_.End.ToString('HH:mm:ss')}},
                            @{n='Mins';e={[math]::Round(($_.End - $_.Start).TotalMinutes,1)}} |
    Format-Table -AutoSize | Out-String -Width 160 | Write-Host

if ($overlaps.Count -eq 0) {
    Write-Host "CLEAN: $($runs.Count) legs, no overlap. Each started after every earlier one finished." -ForegroundColor Green
    exit 0
}

Write-Host 'CONTAMINATED -- these legs shared the box:' -ForegroundColor Red
$overlaps | Format-Table -AutoSize | Out-String -Width 160 | Write-Host
Write-Host ('VOID BOTH SIDES of each overlap, not just the later leg: contention is mutual, and ' +
            'a contended leg reads as a slow configuration with nothing in the log to say so.') -ForegroundColor Red
exit 1
