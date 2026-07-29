# Wave F's decision number: COLD-FILL TIME, 64 m against 128 m.
#
# The plan says Wave F is falsified if the R0 = 128 m prototype's cold fill or
# motion residency is worse than today's. That makes time-to-settle the number,
# not resident counts -- the cascade obviously holds more chunks at 128 m, and
# quoting that as a result would be quoting the input.
#
# SETTLE IS READ FROM THE LOG, NOT A STOPWATCH. A leg is settled at the first
# "Voxel streaming:" line with jobsInFlight=0 AND pendingJobs=0 whose resident
# quad count then stops changing. Timestamps come from the log's own
# [YYYY.MM.DD-HH.MM.SS:mmm] prefix, so the number excludes process startup,
# shader warm-up and map load -- all of which are identical between configs and
# would otherwise swamp a 4x difference in the thing being measured.
#
# Both comma-list switches are QUOTED. Unquoted, PowerShell splits them into an
# array before the process sees them; and until the FParse::Value fix, the
# engine read only the first entry of each. Two different silent truncations on
# the same two switches. Do not unquote them.
#
# Ground rule 1: >=2 legs per config, alternated, and the two same-config
# readings ARE the noise floor. If the configs differ by less than that spread,
# there is no difference to report.
#
# THE SKY PIN. This script's number IS a time-to-settle comparison across
# three sequential leg types (64 m, 128 m, 128 m + GPU fork), each of which can
# run for minutes. Left at the engine's default TimeScale 1, the sun keeps
# moving the whole time, so whichever config happens to be mid-fill when the
# sun crosses a shadow-cascade boundary picks up extra rebuild work that has
# nothing to do with ring size or the fork -- the same "quoting the input"
# trap the settle-vs-lull section below is about, arriving from the render
# side instead of the streaming side. Freeze it so 64 m, 128 m and
# 128 m + fork are all measured under the same static sun.

param(
    [int]$Legs = 2,
    [int]$BudgetSec = 300,
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    # THE SKY PINS. See the header. Same three defaults as
    # tools/voxel-run-flight-leg.ps1.
    [string]$TimeOfDay = '12:00',
    [string]$Date = '03-20',
    [double]$TimeScale = 0
)

$ErrorActionPreference = 'Stop'
$Project = (Resolve-Path "$PSScriptRoot\..\ue-project\VoxelEarth.uproject").Path
$LogDir  = (Resolve-Path "$PSScriptRoot\..\ue-project\Saved\Logs").Path
$EditLog = Join-Path (Split-Path $Project) 'Saved\VoxelWorlds'

function Get-Stamp([string]$line) {
    if ($line -match '^\[(\d{4})\.(\d{2})\.(\d{2})-(\d{2})\.(\d{2})\.(\d{2}):(\d{3})\]') {
        return [datetime]::new($Matches[1],$Matches[2],$Matches[3],$Matches[4],$Matches[5],$Matches[6]).AddMilliseconds([int]$Matches[7])
    }
    return $null
}

function Invoke-ColdFill {
    param([string]$Name, [bool]$R0At128, [bool]$GpuMesh = $false)

    # The edit log persists across runs and replays on load, so a "cold" fill
    # measured without clearing it is not cold. Ground rule 11.
    if (Test-Path $EditLog) { Remove-Item "$EditLog\*.vxlog" -Force -ErrorAction SilentlyContinue }

    $log = Join-Path $LogDir "$Name.log"
    # InvariantCulture on the scale -- see tools/voxel-run-flight-leg.ps1's note
    # on why a comma-decimal machine silently truncates this otherwise.
    $args = @("`"$Project`"", '-game', '-nosplash', '-unattended', '-sm6',
              '-VoxelPerfFlight=static', '-VoxelPerfLogInterval=2', "-abslog=`"$log`"",
              "-VoxelTimeOfDay=$TimeOfDay", "-VoxelDate=$Date",
              "-VoxelTimeScale=$($TimeScale.ToString([cultureinfo]::InvariantCulture))")
    if ($R0At128) {
        $args += '"-VoxelRingInnerMeters=0,128,256,512,1024,2048"'
        $args += '"-VoxelRingOuterMeters=128,256,512,1024,2048,4096"'
    }
    # The third config: same cascade, GPU producer. This is the leg that says
    # whether Wave D actually makes Wave F affordable, which is the entire
    # reason the plan runs A->F in order.
    if ($GpuMesh) { $args += '-VoxelGpuMesh' }

    $p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList $args
    $deadline = (Get-Date).AddSeconds($BudgetSec)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
        Start-Sleep -Seconds 5
        if ((Test-Path $log) -and
            (Select-String -Path $log -Pattern 'jobsInFlight=0 pendingJobs=0' -Quiet)) {
            Start-Sleep -Seconds 6   # let one more window confirm it stayed settled
            break
        }
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

    # Ground rule 12 before any result is read.
    $cl = (Select-String -Path $log -Pattern 'LogInit: Command Line:' | Select-Object -First 1).Line
    $hasGpu = $cl -match 'VoxelGpuMesh'
    if ($GpuMesh -ne $hasGpu) {
        Write-Host "  $Name VOID: -VoxelGpuMesh presence did not match the intended config." -ForegroundColor Red
        return $null
    }
    $has128 = $cl -match 'VoxelRingInnerMeters'
    if ($R0At128 -ne $has128) {
        Write-Host "  $Name VOID: switches did not match the intended config." -ForegroundColor Red
        return $null
    }
    # And prove the cascade RESOLVED to what was asked, not just that the switch
    # was present -- these are different facts, and the parser bug made the
    # second one false while the first stayed true.
    $r0 = (Select-String -Path $log -Pattern 'RingPresets\[0\] = ' | Select-Object -First 1).Line
    $wantOuter = if ($R0At128) { '128.0' } else { '64.0' }
    if ($r0 -notmatch [regex]::Escape("[0.0, $wantOuter)")) {
        Write-Host "  $Name VOID: resolved R0 is not the requested one -> $r0" -ForegroundColor Red
        return $null
    }

    $lines = Select-String -Path $log -Pattern 'Voxel streaming: loaded' | ForEach-Object { $_.Line }
    if ($lines.Count -lt 2) { Write-Host "  ${Name}: no streaming lines" -ForegroundColor Red; return $null }

    $t0 = Get-Stamp $lines[0]

    # SETTLE MEANS FULLY RESIDENT, NOT MERELY IDLE, AND THE DIFFERENCE IS NOT
    # PEDANTRY -- IT INVERTED A RESULT.
    #
    # The first version of this took the first line with
    # "jobsInFlight=0 pendingJobs=0". On the CPU producer that is the same
    # instant as full residency, so it looked correct. With -VoxelGpuMesh it is
    # NOT: the fork produces genuine momentary lulls where nothing is in flight
    # and nothing is queued while the cascade is still incomplete. Measured, on
    # the legs that exposed it:
    #
    #   128 m CPU     settle line loaded=43328, final line loaded=43328  (genuinely settled)
    #   128 m + fork  settle line loaded=40615, final line loaded=42100
    #   128 m + fork  settle line loaded=41229, final line loaded=43328
    #
    # So the fork appeared ~12% faster to fill, and the entire margin was it
    # being scored at 94% of the work. A lull is not a finish line.
    #
    # The fix: settle is the first idle line that has reached the run's PEAK
    # residency. A leg whose peak is itself short of the other config's is
    # reported rather than silently compared -- that is a residency failure, and
    # a different result from a slow fill.
    $peak = ($lines | ForEach-Object { if ($_ -match 'loaded=(\d+)') { [int]$Matches[1] } } | Measure-Object -Maximum).Maximum
    $settled = $lines | Where-Object {
        $_ -match 'jobsInFlight=0 pendingJobs=0' -and $_ -match "loaded=$peak"
    } | Select-Object -First 1
    if (-not $settled) {
        Write-Host "  $Name DID NOT SETTLE within ${BudgetSec}s -- that is itself the result." -ForegroundColor Yellow
        return [pscustomobject]@{ Name=$Name; SettleSec=$null; Quads=$null; Chunks=$null }
    }
    $t1 = Get-Stamp $settled
    $settled -match 'loaded=(\d+).*residentQuads=(\d+)' | Out-Null
    $obj = [pscustomobject]@{
        Name      = $Name
        SettleSec = [math]::Round(($t1 - $t0).TotalSeconds, 1)
        PeakLoaded= $peak
        Chunks    = [int]$Matches[1]
        Quads     = [int64]$Matches[2]
    }
    Write-Host ("  {0}: settle {1}s  chunks {2}  quads {3}  peak {4}" -f $obj.Name,$obj.SettleSec,$obj.Chunks,$obj.Quads,$obj.PeakLoaded) -ForegroundColor Green
    return $obj
}

$results = @()
for ($i = 1; $i -le $Legs; $i++) {
    Write-Host "=== pair $i ===" -ForegroundColor Cyan
    $results += Invoke-ColdFill -Name "f-cold-64-$i"  -R0At128 $false
    $results += Invoke-ColdFill -Name "f-cold-128-$i" -R0At128 $true
    $results += Invoke-ColdFill -Name "f-cold-128gpu-$i" -R0At128 $true -GpuMesh $true
}

Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$results | Where-Object { $_ } | Format-Table -AutoSize
$g64  = $results | Where-Object { $_ -and $_.Name -like '*-64-*'  -and $_.SettleSec }
$g128 = $results | Where-Object { $_ -and $_.Name -like '*-128-*' -and $_.SettleSec }
$gGpu = $results | Where-Object { $_ -and $_.Name -like '*128gpu*' -and $_.SettleSec }
if ($gGpu) { Write-Host ("mean settle 128 m + GPU fork: {0:N1}s" -f (($gGpu.SettleSec | Measure-Object -Average).Average)) }
if ($g64.Count -ge 2) {
    $spread = [math]::Abs($g64[0].SettleSec - $g64[1].SettleSec)
    Write-Host ("64 m same-config spread (the noise floor): {0}s" -f $spread)
}
if ($g64 -and $g128) {
    $m64  = ($g64.SettleSec  | Measure-Object -Average).Average
    $m128 = ($g128.SettleSec | Measure-Object -Average).Average
    Write-Host ("mean settle: 64 m {0:N1}s   128 m {1:N1}s   delta {2:N1}s" -f $m64,$m128,($m128-$m64))
}
