# Wave D / D4 gate. Two legs, per ground rule 1.
#
# TWO PHASES, AND THEY CANNOT SHARE A LAUNCH. This is a trap that cost one leg
# already:
#
#   Phase A  voxel.GPU.VerifyRegion    -- synchronous. Runs to completion inside
#                                         the exec, so `quit` may follow it.
#                                         Carries the determinism digest AND
#                                         D6's band probes.
#   Phase B  voxel.GPU.VerifyAsyncMesh -- ASYNCHRONOUS, and deliberately waits
#                                         before starting so the measurement is
#                                         not taken mid-load. `quit` in the same
#                                         -ExecCmds list executes IMMEDIATELY and
#                                         kills the process while the run is
#                                         still queued. The log then says
#                                         "queued: 16 chunks ..." and stops --
#                                         which reads exactly like a run that
#                                         happened, because every other line in
#                                         the leg is a PASS.
#
# So phase B launches with NO quit and is stopped on a timer, and the script
# FAILS LOUDLY if the completion line never appears rather than reporting the
# PASSes that came before it.
#
# WHY -ExecCmds IS ONE QUOTED STRING AND THE COMMAND LINE IS ECHOED BACK.
# PowerShell splits an unquoted comma list into an array and passes it as
# separate argv entries, so the engine silently receives only the first command.
# The run succeeds, the log looks plausible, and nothing in it says the
# measurement did not happen. First entry on this project's silent-nothing list.

param(
    [int]$Legs = 2,
    [int]$AsyncChunks = 16,
    [int]$AsyncInFlight = 4,
    [int]$AsyncDelaySec = 8,
    [int]$AsyncBudgetSec = 150
)

$ErrorActionPreference = 'Stop'
$Editor  = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Project = (Resolve-Path "$PSScriptRoot\..\ue-project\VoxelEarth.uproject").Path
$LogDir  = (Resolve-Path "$PSScriptRoot\..\ue-project\Saved\Logs").Path
if (-not (Test-Path $Editor)) { throw "Editor not found: $Editor" }

function Assert-CommandLine {
    param([string]$Log, [string[]]$MustContain, [string]$Label)
    $line = (Select-String -Path $Log -Pattern 'LogInit: Command Line:' | Select-Object -First 1).Line
    Write-Host $line -ForegroundColor DarkGray
    foreach ($m in $MustContain) {
        if ($line -notmatch [regex]::Escape($m)) {
            Write-Host "$Label VOID: '$m' did not survive argument passing." -ForegroundColor Red
            return $false
        }
    }
    return $true
}

for ($i = 1; $i -le $Legs; $i++) {
    Write-Host "=== LEG $i / $Legs ===" -ForegroundColor Cyan

    # ---- Phase A: synchronous gates -------------------------------------
    $logA = Join-Path $LogDir "d4-gate-leg$i-region.log"
    & $Editor $Project -game -nosplash -unattended -sm6 `
        -ExecCmds="voxel.GPU.VerifyRegion, quit" -abslog="$logA" 2>&1 | Out-Null

    if (Test-Path $logA) {
        if (Assert-CommandLine -Log $logA -MustContain @('VerifyRegion') -Label "LEG ${i} phase A") {
            Select-String -Path $logA -Pattern 'D6 band|D3 quad-total|D4 quad pack|digest|PASS:|FAIL' |
                ForEach-Object { $_.Line -replace '^\[[^\]]+\]\[[^\]]+\]', '' }
        }
    } else { Write-Host "LEG ${i} phase A: no log" -ForegroundColor Red }

    # ---- Phase B: the async fork gate, no quit, stopped on a timer -------
    $logB = Join-Path $LogDir "d4-gate-leg$i-async.log"
    $p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList @(
        "`"$Project`"", '-game', '-nosplash', '-unattended', '-sm6',
        "-ExecCmds=`"voxel.GPU.VerifyAsyncMesh $AsyncChunks $AsyncInFlight $AsyncDelaySec`"",
        "-abslog=`"$logB`"")

    $deadline = (Get-Date).AddSeconds($AsyncBudgetSec)
    $done = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        if ($p.HasExited) { break }
        # The verdict lines, as the harness actually words them. Matching the
        # wrong pattern here does not produce a wrong answer -- the script says
        # INCONCLUSIVE -- but it does waste the leg, so they are pinned exactly.
        if ((Test-Path $logB) -and (Select-String -Path $logB -Pattern 'PASS: all \d+ chunks|MISMATCH|FAIL' -Quiet)) {
            $done = $true; break
        }
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

    if (Test-Path $logB) {
        if (Assert-CommandLine -Log $logB -MustContain @('VerifyAsyncMesh') -Label "LEG ${i} phase B") {
            Select-String -Path $logB -Pattern 'VerifyAsyncMesh|MISMATCH|chunks/s|dispatch-to-ready' |
                ForEach-Object { $_.Line -replace '^\[[^\]]+\]\[[^\]]+\]', '' }
        }
    }
    if (-not $done) {
        Write-Host "LEG ${i} phase B INCONCLUSIVE: no completion line within ${AsyncBudgetSec}s. Do NOT read the PASSes above as a result." -ForegroundColor Red
    }
}
