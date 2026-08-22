<#
.SYNOPSIS
    Capture the front end -- main menu, its panels, the hourglass, the loading
    screen -- headless and unattended.

.DESCRIPTION
    The sibling of tools/voxel-capture.ps1, for the screens rather than the
    world. It exists because those two capture DIFFERENT things and one script
    doing both would need every switch of each: voxel-capture.ps1 photographs a
    settled world with the UI off, and every shot here needs the UI ON and the
    world irrelevant or absent.

    It keeps that script's discipline, and for the same reasons:

      * ONE EDITOR AT A TIME. A second UnrealEditor-Cmd sharing the derived-data
        cache and the log file turns a capture into a coin flip.
      * A BANNER echoing every switch actually passed, so a surprising image can
        be traced to the command that made it rather than to memory.
      * A BEFORE/AFTER diff of the screenshot directory, so the file this run
        produced is named exactly rather than guessed at by timestamp.
      * A POST-RUN GREP for the two lines that say what the front end decided --
        "VoxelFrontEnd:" and "VoxelLoadGate:" -- printed next to the image. A
        capture can then be believed or discarded on evidence: if the log says
        the front end was suppressed, the picture is of something else.

.EXAMPLE
    tools\voxel-ui-capture.ps1 -Shot Menu
    tools\voxel-ui-capture.ps1 -Shot Panel -Panel load
    tools\voxel-ui-capture.ps1 -Shot Fallback
    tools\voxel-ui-capture.ps1 -Shot Hourglass
    tools\voxel-ui-capture.ps1 -Shot Loading -At '0.5,6,20'
    tools\voxel-ui-capture.ps1 -Shot GateSweep -GateRing 2 -MaxHold 180
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Menu', 'Panel', 'Fallback', 'Hourglass', 'Loading', 'GateSweep')]
    [string]$Shot,

    # -Shot Panel only: which sub-panel to open before the shutter.
    [ValidateSet('load', 'help', 'credits', 'settings')]
    [string]$Panel = 'load',

    # -Shot Loading only: comma-separated seconds to capture at. The default
    # puts the bar mid-fill with a mound formed and grains falling -- an empty
    # hourglass proves nothing about the hourglass.
    [string]$At = '6',

    # -Shot Hourglass only: comma-separated progress values.
    [string]$Progress = '0.0,0.25,0.5,0.75,1.0',

    # -Shot GateSweep only: which ring the readiness gate requires.
    [int]$GateRing = 3,
    [double]$MaxHold = 180,

    [string]$Engine = 'D:/UE5/UE_5.8',
    [string]$Project,
    [int]$Width = 2560,
    [int]$Height = 1440,
    # Settle before the menu shutter. The background art decodes on a worker
    # and glyphs rasterise lazily, so frame one is a half-built menu.
    [double]$SettleSec = 2.0
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Project) { $Project = Join-Path $RepoRoot 'ue-project/VoxelEarth.uproject' }
if (-not (Test-Path $Project)) { throw "No .uproject at $Project" }

$EditorExe = Join-Path $Engine 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
if (-not (Test-Path $EditorExe)) { throw "No editor at $EditorExe (pass -Engine)" }

$SavedDir = Join-Path $RepoRoot 'ue-project/Saved'
$ShotDir = Join-Path $SavedDir 'Screenshots/WindowsEditor'
$LogPath = Join-Path $SavedDir ("ui-capture-{0}.log" -f $Shot.ToLower())

# ONE EDITOR AT A TIME.
$running = Get-Process -Name 'UnrealEditor', 'UnrealEditor-Cmd' -ErrorAction SilentlyContinue
if ($running) {
    $detail = ($running | ForEach-Object { "PID $($_.Id) ($($_.ProcessName))" }) -join ', '
    throw "REFUSING TO START: $($running.Count) editor process(es) already running -- $detail."
}

# Note which shots already exist, so the ones this run produces can be named
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
    # Explicit, even though every -Shot below implies it through
    # kFrontEndCaptureSwitches. Belt and braces on the one setting whose being
    # wrong makes every image in this script a picture of the wrong thing.
    '-VoxelFrontEnd=1'
)

# InvariantCulture on every interpolated double: PowerShell formats with the
# CURRENT culture, so on a comma-decimal machine "-VoxelLoadMaxHold=180,0"
# reaches FParse::Value, which stops at the comma. voxel-capture.ps1 carries
# the same note for -VoxelTimeScale, having been bitten by it.
function Inv([double]$Value) { $Value.ToString([cultureinfo]::InvariantCulture) }

switch ($Shot) {
    'Menu' {
        $argList += "-VoxelMenuShot=$(Inv $SettleSec)"
    }
    'Panel' {
        $argList += @("-VoxelMenuShot=$(Inv $SettleSec)", "-VoxelMenuPanel=$Panel")
        # An empty LOAD list is the interesting case for a fresh checkout, and
        # -VoxelNoLoad guarantees it rather than depending on what happens to
        # be in Saved/.
        if ($Panel -eq 'load') { $argList += '-VoxelNoLoad' }
    }
    'Fallback' {
        # The degraded path: no font, no art. It is the arm nobody exercises
        # until it is the only one they have.
        $argList += @("-VoxelMenuShot=$(Inv $SettleSec)", '-VoxelUINoAssets')
    }
    'Hourglass' {
        $argList += "-VoxelHourglassShot=$Progress"
    }
    'Loading' {
        $argList += "-VoxelLoadingShotAt=$At"
    }
    'GateSweep' {
        # No shutter at all: this arm is a MEASUREMENT, and its output is the
        # VoxelLoadGate lines in the log rather than a picture. Feeds
        # docs/measurements/front-end-gate-<date>.txt.
        $argList += @('-VoxelMenuAutoStart=1', '-VoxelReadyProbeLog',
                      "-VoxelLoadGateMaxRing=$GateRing", "-VoxelLoadMaxHold=$(Inv $MaxHold)",
                      '-VoxelMenuWatchdog=300')
    }
}

Write-Host ''
Write-Host '=== voxel-ui-capture =========================================='
Write-Host "  shot     : $Shot"
Write-Host "  editor   : $EditorExe"
Write-Host "  project  : $Project"
Write-Host "  log      : $LogPath"
Write-Host "  switches : $($argList -join ' ')"
Write-Host '==============================================================='
Write-Host ''

$process = Start-Process -FilePath $EditorExe -ArgumentList $argList -PassThru -Wait -NoNewWindow
Write-Host "editor exited with code $($process.ExitCode)"

# --- What the front end actually decided --------------------------------------
#
# Printed BEFORE the file list, because it is what tells you whether the file
# list is worth reading. "VoxelFrontEnd: suppressed (...)" means the picture is
# of an empty world, not of a menu.
if (Test-Path $LogPath) {
    $verdicts = Select-String -Path $LogPath -Pattern 'VoxelFrontEnd:|VoxelLoadGate:' -ErrorAction SilentlyContinue
    if ($verdicts) {
        Write-Host ''
        Write-Host '--- front-end log lines ---------------------------------------'
        $verdicts | ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
        Write-Host '---------------------------------------------------------------'
    } else {
        Write-Warning "No VoxelFrontEnd: lines in $LogPath -- the module may not have loaded."
    }
} else {
    Write-Warning "No log at $LogPath."
}

# --- Which files this run produced --------------------------------------------
$new = @()
if (Test-Path $ShotDir) {
    $new = Get-ChildItem $ShotDir -Filter *.png -ErrorAction SilentlyContinue |
        Where-Object { -not $before.ContainsKey($_.Name) } |
        Sort-Object LastWriteTime
}
if ($new) {
    Write-Host ''
    Write-Host "captured $($new.Count) image(s):"
    $new | ForEach-Object { Write-Host ("  " + $_.FullName) }
} elseif ($Shot -ne 'GateSweep') {
    Write-Warning "No new screenshot appeared in $ShotDir. Check the log above."
}
