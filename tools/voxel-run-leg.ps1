# Run ONE headless leg and stop it only when the world is GENUINELY settled.
#
# THIS EXISTS BECAUSE THE SAME MISTAKE HAS NOW BEEN MADE TWICE, and both times
# it produced a published number that had to be retracted.
#
# The tempting stop condition is the first log line with
# "jobsInFlight=0 pendingJobs=0". On the CPU producer that coincides with full
# residency, so it looks correct. With the GPU fork it does NOT: the fork
# produces genuine momentary lulls mid-fill, where nothing is in flight and
# nothing is queued while the cascade is still building. Measured, twice:
#
#   * a leg "settled" at loaded=40,615 with a true peak of 43,328 -- the fork was
#     credited with finishing 94% of the work and looked 12% faster than it was
#   * a leg killed at t=38.4 s while loaded was still climbing (41,968 -> 42,625
#     in its final two seconds) and was written up as a residency stall
#
# A LULL IS NOT A FINISH LINE. The only safe stop is "loaded has not changed for
# N consecutive samples AND nothing is in flight AND nothing is pending".
#
# Use this instead of writing another inline wait loop. The inline loop is how
# the fix got bypassed the second time: the correct logic already existed in
# tools/wave-f-coldfill.ps1 and was simply not called.
#
# ============================================================================
# THIS IS A COLD-FILL DRIVER. DO NOT USE IT FOR FLIGHT LEGS.
# ============================================================================
#
# Found in Wave S0 (docs/measurements/s0-apply-census-2026-07-27.txt): passing
# -VoxelPerfFlight=line through this script SILENTLY TRUNCATES THE LEG. The
# world settles during the 90 s preflight, the settle rule below fires, and the
# editor is killed BEFORE THE FLIGHT EVER STARTS -- 32 s before, as measured --
# and the script returns $true. The resulting log is a complete-looking
# cold-fill run with no flight phase in it at all.
#
# It aborts on that combination now rather than trusting the caller to remember.
#
# A flight leg does not need this script, because it does not have this
# script's hazard. UVoxelPerfRunSubsystem calls RequestExit itself at
# PreflightSec + DurationSeconds + LingerSec, so the run's own clock decides
# when it is finished and there is no lull to mistake for a finish line. Launch
# the editor directly and wait for it to exit (306 s observed for a 90/120/60
# leg); kill it past a generous deadline and treat that as VOID.
#
# AND QUOTE -ExecCmds PROPERLY, whichever driver you use. Start-Process
# -ArgumentList does NOT re-quote an argument containing spaces, so
# '-ExecCmds=cvar 1, cvar2 1' reaches UE as -ExecCmds=cvar and everything after
# the first space is dropped. In Wave S0 that presented as every gated timing
# reading 0.00ms -- i.e. "the apply path costs nothing", the exact opposite of
# the truth, in a log that otherwise looked healthy. Embed the quotes:
#   '-ExecCmds="cvar 1, cvar2 1"'

param(
    [Parameter(Mandatory=$true)][string]$LogPath,
    [string[]]$ExtraArgs = @(),
    [int]$BudgetSec = 300,
    # How many consecutive samples must show the SAME loaded count before the
    # world is called settled. Two is enough to reject a single-sample lull;
    # three is cheap insurance and this is not a latency-sensitive decision.
    [int]$StableSamples = 3,
    [int]$PollSec = 3,
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [switch]$ClearEditLog
)

$ErrorActionPreference = 'Stop'
$Project = (Resolve-Path "$PSScriptRoot\..\ue-project\VoxelEarth.uproject").Path

# Refuse the combination that silently truncates (see the header). A flight leg
# driven through the settle rule is not a slow result or a noisy one -- it is a
# leg that stopped before the phase being measured began, and it reports success.
$flightArg = $ExtraArgs | Where-Object { $_ -match '^-VoxelPerfFlight=(.+)$' } | Select-Object -First 1
if ($flightArg -and $flightArg -notmatch '^-VoxelPerfFlight=static$') {
    throw ("voxel-run-leg.ps1 is a COLD-FILL driver and $flightArg would be truncated: " +
           "the world settles during the preflight, this script's settle rule fires, and the editor is " +
           "killed before the flight starts -- returning success. Launch the editor directly for flight " +
           "legs and wait for UVoxelPerfRunSubsystem to exit on its own clock " +
           "(PreflightSec + VoxelPerfRun + LingerSec). See docs/measurements/s0-apply-census-2026-07-27.txt.")
}

# The edit log persists across runs and replays on load, so a "cold" fill
# measured without clearing it is not cold (ground rule 11).
if ($ClearEditLog) {
    $dir = Join-Path (Split-Path $Project) 'Saved\VoxelWorlds'
    if (Test-Path $dir) {
        Get-ChildItem $dir -Filter *.vxlog -ErrorAction SilentlyContinue | Remove-Item -Force
    }
}

$argList = @("`"$Project`"", '-game', '-nosplash', '-unattended', '-sm6', "-abslog=`"$LogPath`"") + $ExtraArgs
$p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList $argList

$deadline = (Get-Date).AddSeconds($BudgetSec)
$lastLoaded = -1
$stable = 0
$settled = $false

while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    Start-Sleep -Seconds $PollSec
    if (-not (Test-Path $LogPath)) { continue }

    $line = Select-String -Path $LogPath -Pattern 'Voxel streaming: loaded' |
            Select-Object -Last 1
    if (-not $line) { continue }
    if ($line.Line -notmatch 'loaded=(\d+).*jobsInFlight=(\d+) pendingJobs=(\d+)') { continue }

    $loaded  = [int]$Matches[1]
    $inFlight= [int]$Matches[2]
    $pending = [int]$Matches[3]

    if ($loaded -eq $lastLoaded -and $inFlight -eq 0 -and $pending -eq 0) {
        $stable++
        if ($stable -ge $StableSamples) { $settled = $true; break }
    } else {
        $stable = 0
        $lastLoaded = $loaded
    }
}

if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

if (-not $settled) {
    Write-Host "  $(Split-Path $LogPath -Leaf): DID NOT SETTLE in ${BudgetSec}s -- treat as VOID, not as a slow result." -ForegroundColor Yellow
}
return $settled
