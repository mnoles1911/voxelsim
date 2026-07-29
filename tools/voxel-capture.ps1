# Capture ONE settled screenshot of the world, headless and unattended.
#
# WHY THIS EXISTS AS A FILE. Worldgen changes are judged on instruments here --
# vxc_terrainprobe, vxc_bench digests, the leg harness -- and that is right,
# because screenshots have twice failed to catch what the probe caught. But the
# converse also happened this session: three of the v10 detail terms measured as
# "no effect" for hours because the METRIC was binned on the wrong quantity. An
# instrument you have not sanity-checked against a picture is not obviously
# better than a picture. So: both, and this is the picture half, as a committed
# file rather than a pasted command line.
#
# THE ONE THING THAT MAKES CAPTURES LIE. A blank or half-empty frame is almost
# never a rendering bug -- it is terrain that has not streamed in yet. Capture
# too early and you photograph an empty world and then go debugging the mesher.
# So this script does two things about it:
#   * waits -SettleSec before capturing (the engine-side timer is
#     -VoxelScreenshotAfter, which UE runs off the world timer, so it is real
#     game time and not wall clock), and
#   * AFTERWARDS greps the log for the streaming counters and prints them next
#     to the file it wrote, so the capture can be believed or thrown away on
#     evidence rather than on how it looks.
#
# It also inherits the one-editor rule from voxel-run-flight-leg.ps1, for the
# same reason: a second editor on the box does not corrupt a screenshot the way
# it corrupts a timing, but it WILL make this script's own settle wait
# meaningless, and it can trip the other run.
#
# Usage:
#   tools\voxel-capture.ps1 -Name v10-alpine -SpawnAt '-65000,60000'
#   tools\voxel-capture.ps1 -Name v10-wide  -SpawnAt '-84480,53760' -SettleSec 150

param(
    [Parameter(Mandatory=$true)][string]$Name,
    # Defaults to the tile-coverage centre. OFF the generated tiles you are
    # photographing the flat fallback plane, which looks like a bug and is not.
    [string]$SpawnAt = '-84480,53760',
    # Long by default. The flight legs settle during a 90 s preflight, and this
    # is the one parameter where being wrong is silent.
    [int]$SettleSec = 120,
    [int]$Width = 2560,
    [int]$Height = 1440,
    [int]$TimeoutSec = 420,
    [string]$Cvars = '',
    [string[]]$ExtraArgs = @(),
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
)

$ErrorActionPreference = 'Stop'
$Root    = (Resolve-Path "$PSScriptRoot\..").Path
$Project = (Resolve-Path "$Root\ue-project\VoxelEarth.uproject").Path
$LogPath = Join-Path $Root "Saved\capture-$Name.log"
$ShotDir = Join-Path $Root 'ue-project\Saved\Screenshots\WindowsEditor'

$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id) ($($_.ProcessName))" }) -join ', '
    throw "REFUSING TO START: $($running.Count) editor process(es) already running -- $detail."
}

# Note which shots already exist, so the one this run produces can be named
# exactly rather than guessed at by timestamp.
$before = @{}
if (Test-Path $ShotDir) {
    Get-ChildItem $ShotDir -Filter *.png -ErrorAction SilentlyContinue |
        ForEach-Object { $before[$_.Name] = $true }
}

$argList = @(
    "`"$Project`"", '-game', '-nosplash', '-unattended', '-sm6', '-dx12',
    "-abslog=`"$LogPath`"",
    "-ResX=$Width", "-ResY=$Height", '-WinX=0', '-WinY=0',
    "-VoxelSpawnAt=$SpawnAt",
    "-VoxelScreenshotAfter=$SettleSec"
)
if ($Cvars) { $argList += "-ExecCmds=`"$Cvars`"" }   # embed the quotes; see the leg script
$argList += $ExtraArgs

Write-Host "capture '$Name' at $SpawnAt, settling ${SettleSec}s, ${Width}x${Height}" -ForegroundColor Cyan
$started = Get-Date
$p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList $argList
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    Write-Warning "editor still alive after ${TimeoutSec}s -- killing. Capture may still be valid."
    try { $p.Kill() } catch {}
}
$elapsed = [int]((Get-Date) - $started).TotalSeconds

$shot = $null
if (Test-Path $ShotDir) {
    $shot = Get-ChildItem $ShotDir -Filter *.png -ErrorAction SilentlyContinue |
        Where-Object { -not $before.ContainsKey($_.Name) } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

# THE EVIDENCE, not the vibe. A capture with a small loaded-chunk count is a
# photograph of an empty world; print the numbers next to the file so that is
# decidable without opening the image.
if (Test-Path $LogPath) {
    $loaded = Select-String -Path $LogPath -Pattern 'loaded=(\d+)' -AllMatches |
        ForEach-Object { $_.Matches } | ForEach-Object { [int]$_.Groups[1].Value }
    $peak = if ($loaded) { ($loaded | Measure-Object -Maximum).Maximum } else { 0 }
    $final = if ($loaded) { $loaded[-1] } else { 0 }
    Write-Host ("  streaming: peak loaded={0}, final={1}, samples={2}" -f
                $peak, $final, $loaded.Count)
    if ($peak -lt 1000) {
        Write-Warning ("peak loaded=$peak is LOW -- this is very likely a photograph of " +
                       "terrain that had not streamed in. Raise -SettleSec, or check the " +
                       "spawn is inside the generated tile set, before believing the image.")
    }
    $errs = Select-String -Path $LogPath -Pattern 'Fatal|Assertion failed' | Select-Object -First 3
    if ($errs) { Write-Warning "log contains fatal lines:"; $errs | ForEach-Object { Write-Host "    $_" } }
}

if ($shot) {
    Write-Host "  wrote $($shot.FullName) after ${elapsed}s" -ForegroundColor Green
    $shot.FullName
} else {
    throw "no new screenshot appeared in $ShotDir after ${elapsed}s -- see $LogPath"
}
