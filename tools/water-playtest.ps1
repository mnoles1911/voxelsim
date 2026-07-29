# water-playtest.ps1 -- launch the water build for a hands-on design pass.
#
# Interactive, windowed, real RHI. NOT a measurement harness: it deliberately
# does not pass -unattended (which would suppress debug draws) and does not
# pour anything automatically, because the point is that YOU pour it, wherever
# you are looking, and watch what it does.
#
# BASELINE WORLD BY DEFAULT. Both the edit log (craters, digs) and the water
# blob (every bucket ever poured) persist across sessions -- that is ADR-0005
# and the M3 edit log doing exactly what they are supposed to do. For a GAME
# that is correct. For a TEST HARNESS it is not: it means run N starts from
# whatever run N-1 left behind, so two captures of "the same" scene are not
# comparable and a benchmark drifts as the log grows.
#
# This bit us already. An early water time-lapse silently accumulated a pour
# per run -- ledger volume 150k, 180k, 210k, 240k -- and the frames looked like
# a settling sequence when they were really four different volumes. And one
# play-test session grew the edit log from 31 bytes to 732 KB of craters, every
# one of which the next launch would have replayed.
#
# So the default is a pristine world, and keeping state is the flag you have to
# ask for. It also always says what it discarded: silently deleting a player's
# dug-out base would be its own kind of bug.
#
# Usage:
#   .\tools\water-playtest.ps1                 # BASELINE: fresh world, tile-centre terrain
#   .\tools\water-playtest.ps1 -Keep           # keep craters + water from last session
#   .\tools\water-playtest.ps1 -Origin         # spawn at (0,0), which is ~sea level
#   .\tools\water-playtest.ps1 -Swe            # arm the SWE momentum layer at boot
[CmdletBinding()]
param(
    [switch]$Origin,
    [switch]$Keep,
    [switch]$Swe
)

$ErrorActionPreference = 'Stop'

$Root    = Split-Path -Parent $PSScriptRoot
$UProject = Join-Path $Root 'ue-project\VoxelEarth.uproject'
$Editor  = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
# The two halves of persisted world state. Seed 20260719 is
# UVoxelWorldSubsystem::DefaultSeed; both files are named after it.
#   .vxlog   -- the M3 edit log: every dig, place and charge crater, replayed
#               on load. This is the one that silently grows across sessions.
#   .vxwater -- ADR-0005's CA fill blob: irreducible simulation state that is
#               NOT re-derivable from seed + edit log, which is exactly why it
#               is persisted separately.
$WaterSave = Join-Path $Root 'ue-project\Saved\VoxelWorlds\20260719.vxwater'
$EditLog   = Join-Path $Root 'ue-project\Saved\VoxelWorlds\20260719.vxlog'

if (-not (Test-Path $Editor))   { throw "Editor not found: $Editor" }
if (-not (Test-Path $UProject)) { throw "Project not found: $UProject" }

# Refuse to start beside another editor. Two of them fight over the same
# worktree's Saved/ and DerivedDataCache, and it is also how a previous session
# lost 2h40m to a zombie that was quietly holding the project.
$live = @(Get-Process UnrealEditor, UnrealEditor-Cmd -ErrorAction SilentlyContinue)
if ($live.Count -gt 0) {
    $live | Select-Object Name, Id, StartTime | Format-Table -AutoSize | Out-String | Write-Host
    throw "An editor is already running. Close it (or Stop-Process -Id <id>) and re-run."
}

if ($Keep) {
    $kept = @()
    foreach ($f in @($EditLog, $WaterSave)) {
        if (Test-Path $f) { $kept += ("{0} ({1:N0} B)" -f (Split-Path $f -Leaf), (Get-Item $f).Length) }
    }
    if ($kept.Count -gt 0) {
        Write-Host "KEEPING previous session state: $($kept -join ', ')" -ForegroundColor Yellow
        Write-Host "  Captures from this run are NOT comparable to a baseline run." -ForegroundColor Yellow
    } else {
        Write-Host '-Keep passed, but there was no saved state to keep.' -ForegroundColor DarkGray
    }
} else {
    # Report sizes before deleting. "Discarded 732 KB of edit log" is the line
    # that tells you the last session actually did something, and it is the
    # only warning you get that a dug-out base just went away.
    $dropped = @()
    foreach ($f in @($EditLog, $WaterSave)) {
        if (Test-Path $f) {
            $dropped += ("{0} ({1:N0} B)" -f (Split-Path $f -Leaf), (Get-Item $f).Length)
            Remove-Item $f -Force
        }
    }
    if ($dropped.Count -gt 0) {
        Write-Host "BASELINE WORLD -- discarded $($dropped -join ', ')" -ForegroundColor Green
        Write-Host '  Pass -Keep to carry craters and water forward instead.' -ForegroundColor DarkGray
    } else {
        Write-Host 'BASELINE WORLD -- nothing to discard, already pristine.' -ForegroundColor Green
    }
}

$args = @(
    $UProject,
    '-game', '-sm6', '-dx12',
    '-windowed', '-ResX=1600', '-ResY=900'
)

# The tile cache does not cover the origin, so spawning there gives the flat
# missing-tile sea-level default rather than real diffusion terrain. The tile
# centre is high ground (surface ~545 m), which also keeps the implicit ocean
# out of frame -- so any water you see is water YOU poured.
if (-not $Origin) { $args += '-VoxelSpawnAt=-84480,53760' }

# W3 rivers (plan S3.7 Layer R). Unlike -Swe, arming late is harmless: the
# graph is built on whatever Tick the cvar first reads as 1, so if -ExecCmds
# lands after the world has loaded (it usually does), the only consequence is
# that the river starts a second later. Typing `voxel.Water.Rivers 1` in the
# console does exactly the same thing and is the reliable path.
$exec = @()
if ($Swe)    { $exec += 'voxel.Water.SWE 1' }
if ($Rivers) { $exec += 'voxel.Water.Rivers 1' }
if ($exec.Count -gt 0) { $args += ('-ExecCmds=' + ($exec -join '|')) }

Write-Host ''
Write-Host 'Launching. Once you are in:' -ForegroundColor Cyan
Write-Host '  ~                     open the console'
Write-Host '  voxel.SpawnWater 30000     a bucket   (~118 L, ~1 m puddle)'
Write-Host '  voxel.SpawnWater 200000    a bathtub'
Write-Host '  voxel.SpawnWater 1000000   a small pond'
Write-Host '                        ...pours AT YOUR CROSSHAIR, so aim first.'
Write-Host ''
Write-Host '  voxel.Water.GPU 0 / 1      component vs pooled render path (A/B)'
Write-Host '  voxel.Water.SWE 0 / 1      momentum layer off / on'
Write-Host ''
Write-Host '  voxel.Water.Rivers 1       W3: build the river graph HERE and start it flowing.'
Write-Host '                        Watch LogVoxelWater: an arm line naming the node/segment'
Write-Host '                        count, then a RiverPerf: heartbeat every 5 s. Water appears'
Write-Host '                        at each reach outlet within a second or two and runs downhill.'
Write-Host '                        Dig a ditch out of a reach and keep it flowing for ~30 s to'
Write-Host '                        see a PROMOTED line -- the CA-to-graph half.'
Write-Host '                        The graph is built ONCE, around where you were standing.'
Write-Host '  stat unit                  read DRAW, not Frame'
Write-Host '  G  walk/fly    V  1st/3rd person    F1  overlay    F3  debug'
Write-Host ''

& $Editor @args
