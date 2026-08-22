# PHASE 0 SWEEP -- runs the remaining Arm A / GATE 0 legs SERIALLY and prints one table.
#
# WHY A DRIVER AND NOT A LOOP IN THE SHELL. One editor per box is a hard rule on
# this project, and the two ways it gets broken are (a) launching a second leg
# while the first is still exiting, and (b) a leg that dies early looking exactly
# like a leg that ran. This waits on each child, records VOID separately from a
# real result, and refuses to summarise a leg the runner voided -- the same
# discipline tools/voxel-leg-summary.ps1 enforces with -MinWindows.
#
# ALTERNATED, NEVER GROUPED. Arms run A,B,A,B rather than AA,BB so wall-clock
# drift (thermals, background load, shader cache warming) hits both arms equally.
# tools/voxel-t41-lead-sweep.ps1 established that shape here.
param(
    [Parameter(Mandatory=$true)][hashtable[]]$Arms,   # @{ Name='x'; Cvars='...'; Width=2560; Height=1440 }
    [int]$Reps = 1,
    [string]$SpawnAt = '-61440,-61440',
    [string]$Flight = 'static',
    [int]$RunSec = 150,
    [int]$PreflightSec = 90
)
$results = @()
for ($r = 1; $r -le $Reps; $r++) {
    foreach ($a in $Arms) {
        $extra = if ($a.ExtraArgs) { $a.ExtraArgs } else { @() }
        $w = if ($a.Width)  { $a.Width }  else { 2560 }
        $h = if ($a.Height) { $a.Height } else { 1440 }
        $log = "p0-$($a.Name)-r$r"
        Write-Host "[$([DateTime]::Now.ToString('HH:mm:ss'))] $log  ${w}x${h}" -ForegroundColor Cyan
        $ok = & "$PSScriptRoot\voxel-run-gpu-arm.ps1" -LogName $log -SpawnAt $SpawnAt -Flight $Flight `
                -RunSec $RunSec -PreflightSec $PreflightSec -CaptureAt ($PreflightSec + [int]($RunSec/2)) `
                -Width $w -Height $h -Cvars $a.Cvars -ExtraArgs $extra
        $line = Select-String -Path "D:\voxelsim\Saved\$log.log" -Pattern 'post-warmup' -EA SilentlyContinue |
                Select-Object -Last 1
        $p50 = $null; $p95 = $null; $frames = $null
        if ($line -and $line.Line -match 'frames=(\d+) p50=([\d.]+)ms p95=([\d.]+)ms') {
            $frames = [int]$Matches[1]; $p50 = [double]$Matches[2]; $p95 = [double]$Matches[3]
        }
        $results += [pscustomobject]@{
            Arm = $a.Name; Rep = $r; Res = "${w}x${h}"
            Status = if ($ok) { 'ok' } else { 'VOID' }
            p50 = $p50; p95 = $p95; Frames = $frames; Log = $log
        }
    }
}
""; "==== PHASE 0 SWEEP ===="
$results | Format-Table -AutoSize
$void = @($results | Where-Object { $_.Status -ne 'ok' })
if ($void.Count) { Write-Host "$($void.Count) VOID leg(s) -- excluded from any conclusion." -ForegroundColor Yellow }
$results | Where-Object { $_.Status -eq 'ok' } | Group-Object Arm | ForEach-Object {
    $v = @($_.Group.p50 | Where-Object { $_ -ne $null })
    if ($v.Count) { "  {0,-22} p50 median {1:N2} ms  over {2} rep(s)" -f $_.Name, ($v | Measure-Object -Average).Average, $v.Count }
}
