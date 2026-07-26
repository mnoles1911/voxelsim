# Does the D4 fork actually take chunks in a LIVE streaming session?
#
# The gate (wave-d4-gate.ps1) proves the GPU mesher produces byte-identical
# quads. It says nothing about whether DispatchJobs ever chooses it -- the fork
# has four conditions and any one of them silently excluding every chunk would
# leave a run that streams perfectly, looks identical, and forks NOTHING.
# That is the shape this project keeps getting caught by, so this leg exists to
# make the fork prove it fired.
#
# The decisive line is "Voxel GPU mesh fork (5s window): dispatched=N ..." with
# N > 0, which is printed ONLY when -VoxelGpuMesh is on. failed>0 is the other
# line to read: a failed GPU job delivers an EMPTY chunk, which on screen is
# indistinguishable from terrain that is genuinely empty.
#
# Run A/B: with the fork and without, same anchor, same duration.

param(
    [int]$RunSeconds = 70,
    [string]$Pose = '-VoxelPerfFlight=static'
)

$ErrorActionPreference = 'Stop'
$Editor  = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Project = (Resolve-Path "$PSScriptRoot\..\ue-project\VoxelEarth.uproject").Path
$LogDir  = (Resolve-Path "$PSScriptRoot\..\ue-project\Saved\Logs").Path

function Invoke-Leg {
    param([string]$Name, [bool]$Fork)

    $log = Join-Path $LogDir "d4-fork-$Name.log"
    $args = @("`"$Project`"", '-game', '-nosplash', '-unattended', '-sm6', $Pose,
              '-VoxelPerfLogInterval=5', "-abslog=`"$log`"")
    if ($Fork) { $args += '-VoxelGpuMesh' }

    Write-Host "=== $Name (fork=$Fork) ===" -ForegroundColor Cyan
    $p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList $args
    $deadline = (Get-Date).AddSeconds($RunSeconds)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Seconds 5 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 3 }

    if (-not (Test-Path $log)) { Write-Host "  no log" -ForegroundColor Red; return }

    # Ground rule 12: prove the switch survived before reading any result.
    $cl = (Select-String -Path $log -Pattern 'LogInit: Command Line:' | Select-Object -First 1).Line
    $sawSwitch = $cl -match 'VoxelGpuMesh'
    if ($Fork -and -not $sawSwitch) {
        Write-Host "  VOID: -VoxelGpuMesh did not survive argument passing." -ForegroundColor Red
        return
    }
    if (-not $Fork -and $sawSwitch) {
        Write-Host "  VOID: control leg was handed -VoxelGpuMesh." -ForegroundColor Red
        return
    }

    Select-String -Path $log -Pattern 'GPU mesh fork ENABLED|GPU mesh fork \(5s|cold-band throttle \(5s|VoxelGpuMesh:' |
        ForEach-Object { $_.Line -replace '^\[[^\]]+\]\[[^\]]+\]', '' } | Select-Object -Last 12

    if ($Fork) {
        $fired = Select-String -Path $log -Pattern 'GPU mesh fork \(5s window\): dispatched=([1-9]\d*)'
        if (-not $fired) {
            Write-Host "  FORK NEVER FIRED: dispatched=0 in every window. The wiring is present and inert." -ForegroundColor Red
        } else {
            Write-Host "  fork fired in $($fired.Count) window(s)" -ForegroundColor Green
        }
    }
}

Invoke-Leg -Name 'control' -Fork $false
Invoke-Leg -Name 'gpumesh' -Fork $true
