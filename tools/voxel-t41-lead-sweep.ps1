# T4-1 speculative-lead sweep: run the arms ALTERNATED, two legs each.
#
# WHY ALTERNATED AND NOT GROUPED. Legs drift. Thermals, background work and
# whatever Windows decided to do at 04:00 all move the number, and a sweep that
# runs arm A twice then arm B twice attributes that drift to the arm. Alternating
# means a slow patch of wall-clock hits both arms roughly equally. Ground rule 1
# says never conclude from a single run; this is how the second run is made worth
# having.
#
# Each leg goes through voxel-run-flight-leg.ps1, which refuses to start while
# another editor is alive -- so a leg that overruns delays the sweep instead of
# contending with it. A VOID leg (short exit) is reported and the sweep CONTINUES:
# a missing arm is visible in the summary, a silently retried one is not.
#
# Usage:
#   tools\voxel-t41-lead-sweep.ps1 -Leads 0,1,2,4,8 -Passes 2

param(
    [double[]]$Leads = @(0, 1, 2, 4, 8),
    [int]$Passes = 2,
    [string]$Prefix = 't41',
    [string]$ExtraCvars = '',
    [ValidateSet('line','surface')][string]$Flight = 'line'
)

$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'voxel-run-flight-leg.ps1'
$legs = @()

for ($pass = 1; $pass -le $Passes; $pass++) {
    foreach ($lead in $Leads) {
        # '4' and '0.5' both need to survive into a log name.
        $tag = ($lead.ToString('0.##')) -replace '\.', 'p'
        $name = "$Prefix-lead$tag-$pass"
        $cvars = "voxel.Stream.CoverageVerify 1, voxel.Stream.VelocityLeadSec $lead"
        if ($ExtraCvars) { $cvars = "$cvars, $ExtraCvars" }

        Write-Host "[$([datetime]::Now.ToString('HH:mm:ss'))] leg $name (lead ${lead}s, pass $pass)" -ForegroundColor Cyan
        $ok = $false
        try {
            $ok = & $runner -LogName $name -Cvars $cvars -Flight $Flight
        } catch {
            # Almost always the one-editor guard. Report it and keep going: the
            # summary will show the arm missing, which is the honest outcome.
            Write-Host "  $name FAILED TO START: $($_.Exception.Message)" -ForegroundColor Red
        }
        $legs += [pscustomobject]@{ Name = $name; Lead = $lead; Pass = $pass; Ok = [bool]$ok }
    }
}

Write-Host ''
Write-Host 'Sweep complete. Legs:' -ForegroundColor Green
$legs | Format-Table -AutoSize | Out-String -Width 160 | Write-Host

$good = @($legs | Where-Object { $_.Ok } | ForEach-Object { $_.Name })
if ($good.Count -gt 0) {
    Write-Host 'Summarising completed legs (partial/void legs are excluded by the summariser):' -ForegroundColor Green
    & (Join-Path $PSScriptRoot 'voxel-leg-summary.ps1') -LogName $good
}
