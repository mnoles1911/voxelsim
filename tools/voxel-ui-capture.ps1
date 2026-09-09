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
    tools\voxel-ui-capture.ps1 -Shot Pause
    tools\voxel-ui-capture.ps1 -Shot Pause -Panel save
    tools\voxel-ui-capture.ps1 -Shot Screen
    tools\voxel-ui-capture.ps1 -Shot Screen -Panel map
    tools\voxel-ui-capture.ps1 -Shot Death
    tools\voxel-ui-capture.ps1 -Shot Dialogue
    tools\voxel-ui-capture.ps1 -Shot Hud
    tools\voxel-ui-capture.ps1 -Shot GateSweep -GateRing 2 -MaxHold 180
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Menu', 'Panel', 'Fallback', 'Hourglass', 'Loading', 'GateSweep', 'Pause',
                 'Screen', 'Death', 'Dialogue', 'Hud')]
    [string]$Shot,

    # -Shot Panel: which title-screen sub-panel to open before the shutter.
    # -Shot Pause: which of the pause overlay's four screens.
    # -Shot Screen: which of the five in-game screens.
    [ValidateSet('load', 'help', 'credits', 'settings', 'pause', 'save',
                 'map', 'journal', 'inventory', 'player', 'codex')]
    [string]$Panel = 'load',

    # -Shot Loading only: comma-separated seconds to capture at. The default
    # puts the bar mid-fill with a mound formed and grains falling -- an empty
    # hourglass proves nothing about the hourglass.
    [string]$At = '6',

    # -Shot Hourglass only: comma-separated progress values.
    [string]$Progress = '0.0,0.25,0.5,0.75,1.0',

    # Put a fabricated set of saves in front of the LOAD list and the save
    # dialog. A capture box has no saves, so without this every picture of the
    # LOAD dialog is a picture of its empty state -- no tags, no filter chips
    # doing anything, and no way to see the overwrite band at all. Also
    # suppresses -VoxelNoLoad on `-Shot Panel -Panel load`, which exists to
    # guarantee the opposite.
    [switch]$DemoSaves,

    # Draw the HUD's health and hunger bars, and its interaction prompt, at
    # fabricated values. Same job as -DemoSaves and the same justification:
    # this game has no health, hunger or interaction system, so the HUD gates
    # all three off in play -- a full health bar is a claim, not a decoration --
    # and without this every picture of the HUD is a picture of its empty
    # state. Implied by -Shot Hud, which exists to be reviewed against the
    # mock; pass -NoDemoVitals to photograph the honest in-play HUD instead.
    [switch]$DemoVitals,
    [switch]$NoDemoVitals,
    # 0..100, comma-separated: hp,hunger,wound. Default is the HUD mock's own
    # TWEAK_DEFAULTS.
    [string]$Vitals = '100,100,0',

    # Hard ceiling on the whole run, seconds. NOT a tuning knob -- it is the
    # backstop that stops a finished-but-unexited editor holding the box all
    # night. Generous on purpose: a stock cold start is ~45 s and the load gate
    # can legitimately wait 60 s or more, so this only ever fires on a genuine
    # runaway. See the note at the launch site.
    [int]$TimeoutSec = 600,

    # -Shot GateSweep only: which ring the readiness gate requires.
    [int]$GateRing = 3,
    [double]$MaxHold = 180,

    # The real install on this box (matches voxel-capture.ps1's default).
    # The original 'D:/UE5/UE_5.8' was a cloud-session guess with no engine
    # to check against.
    [string]$Engine = 'D:/UE_5.8',
    [string]$Project,
    # THE CAPTURE'S RESOLUTION, and -ResX/-ResY are aliases because that is what
    # the engine switches are called and what a caller reaches for.
    #
    # A REQUEST HERE WAS INERT UNTIL 2026-09-08 AND SAID NOTHING. The default
    # game window is BORDERLESS FULLSCREEN (Saved/Config/WindowsEditor/
    # GameUserSettings.ini carries FullscreenMode=1), and borderless fullscreen
    # is by definition the desktop's resolution -- so -ResX=1920 -ResY=1080 was
    # accepted, echoed in the banner, and produced a 2560x1440 image. See the
    # -windowed decision and the size check at the bottom of this script: the
    # request now takes, and a run whose image is not the requested size says so
    # instead of handing back a picture of a different screen.
    [Alias('ResX')]
    [int]$Width = 2560,
    [Alias('ResY')]
    [int]$Height = 1440,
    # Settle before the menu shutter. The background art decodes on a worker
    # and glyphs rasterise lazily, so frame one is a half-built menu.
    [double]$SettleSec = 2.0,

    # Extra engine switches, appended verbatim -- mirrors voxel-capture.ps1's
    # parameter of the same name. Added for the colour probe: the SRGBTint
    # A/B needs voxel.UI.SRGBTint=0 SET BEFORE THE MENU PAINTS, and
    # -dpcvars applies at engine init where -ExecCmds may land after the
    # style has already baked its colours. Example:
    #   toolsoxel-ui-capture.ps1 -Shot Menu -ExtraArgs '-dpcvars=voxel.UI.SRGBTint=0'
    [string[]]$ExtraArgs = @()
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
        if ($Panel -eq 'load' -and -not $DemoSaves) { $argList += '-VoxelNoLoad' }
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
    'Pause' {
        # THE ONE SHOT THAT NEEDS A WORLD. The pause overlay only exists after
        # hand-off, so this arm drives NEW GAME, skips the load theatre, waits
        # out the settle twice (once for the world, once for the overlay's
        # glyphs) and photographs whichever of the four screens -Panel names.
        #
        # -VoxelSpawnAt IS MANDATORY, not a tuning knob: the world origin has no
        # fine tiles and the spawn gate is fatal there. -61440,-61440 is the
        # column every in-game capture in this repository uses.
        $argList += @("-VoxelPauseShot=$(Inv $SettleSec)", '-VoxelLoadTheatre=0',
                      '-VoxelSpawnAt=-61440,-61440')
        # -Panel's default is 'load', which is right for -Shot Panel and wrong
        # here; an unspecified pause shot wants the pause list itself.
        $pausePanel = if ($PSBoundParameters.ContainsKey('Panel')) { $Panel } else { 'pause' }
        $argList += "-VoxelPausePanel=$pausePanel"
    }
    'Screen' {
        # THE FOUR ARMS BELOW ALL NEED A WORLD, exactly as 'Pause' does, and
        # carry the same mandatory -VoxelSpawnAt for the same reason: the world
        # origin has no fine tiles and the spawn gate is fatal there.
        $argList += @("-VoxelScreenShot=$(Inv $SettleSec)", '-VoxelLoadTheatre=0',
                      '-VoxelSpawnAt=-61440,-61440')
        # -Panel's default is 'load', which belongs to -Shot Panel; an
        # unspecified screen shot wants INVENTORY, the one tab with a real
        # backing system behind it.
        $screenPanel = if ($PSBoundParameters.ContainsKey('Panel')) { $Panel } else { 'inventory' }
        $argList += "-VoxelScreenPanel=$screenPanel"
    }
    'Death' {
        $argList += @("-VoxelDeathShot=$(Inv $SettleSec)", '-VoxelLoadTheatre=0',
                      '-VoxelSpawnAt=-61440,-61440')
    }
    'Dialogue' {
        $argList += @("-VoxelDialogueShot=$(Inv $SettleSec)", '-VoxelLoadTheatre=0',
                      '-VoxelSpawnAt=-61440,-61440')
    }
    'Hud' {
        # Opens nothing: the HUD installs itself as soon as the player has a
        # pawn, so this arm only has to reach Playing and wait out the settle.
        $argList += @("-VoxelHudShot=$(Inv $SettleSec)", '-VoxelLoadTheatre=0',
                      '-VoxelSpawnAt=-61440,-61440')
        # The vitals bars are the half of this screen the owner is reviewing
        # and the half nothing can fill, so this shot demos them by default --
        # the mirror of `-Shot Panel -Panel load` passing -VoxelNoLoad to
        # guarantee the opposite.
        if (-not $NoDemoVitals) { $DemoVitals = $true }
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

# -windowed WHEN, AND ONLY WHEN, THE REQUEST IS NOT THE DESKTOP'S OWN SIZE.
# Borderless fullscreen cannot be any size but the desktop's, so a smaller
# request is only reachable in a real window. The default (2560x1440 on this
# box) therefore keeps the borderless path every previous capture was taken on
# -- a windowed 2560x1440 is NOT the same picture, because the window chrome
# eats ~48 px of height, the shortest side becomes 1392, and the engine's UI
# scale curve reads 1.29 instead of 1.333 (see docs/adr/0011-scale-tolerant-ui.md).
$DesktopW = 0; $DesktopH = 0
try {
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $DesktopW = $bounds.Width; $DesktopH = $bounds.Height
} catch { }
$Windowed = ($DesktopW -gt 0) -and (($Width -ne $DesktopW) -or ($Height -ne $DesktopH))
if ($Windowed) { $argList += '-windowed' }

if ($DemoSaves) { $argList += '-VoxelDemoSaves' }
if ($DemoVitals) { $argList += "-VoxelDemoVitals=$Vitals" }
if ($ExtraArgs) { $argList += $ExtraArgs }

Write-Host ''
Write-Host '=== voxel-ui-capture =========================================='
Write-Host "  shot     : $Shot"
Write-Host "  editor   : $EditorExe"
Write-Host "  project  : $Project"
Write-Host "  log      : $LogPath"
Write-Host "  requested: ${Width}x${Height}$(if ($Windowed) { ' (windowed)' } else { ' (borderless fullscreen = desktop size)' })"
Write-Host "  desktop  : $(if ($DesktopW) { "${DesktopW}x${DesktopH}" } else { 'unknown' })"
Write-Host "  switches : $($argList -join ' ')"
Write-Host '==============================================================='
Write-Host ''

# BOUNDED, NOT `-Wait`. On 2026-08-25 a `-Shot Loading -At '2,50'` run held the
# box for 241 MINUTES and cost another session two queued jobs. It was not hung:
# both shots fired, the log was still growing at 45 MB, and it had rendered
# 1.58 million settled frames. It had finished its work and was never told to
# stop, and `-Wait` waits forever for that quite happily.
#
# THE ROOT DEFECT: -VoxelLoadingShotAt does not quit after its last shot when
# that shot lands AFTER hand-off. The front end tears down at hand-off and takes
# whatever ends the run with it -- so the image is produced and the process
# lives on. `-At '1,5'` (both shots inside the loading screen) exits 0 cleanly;
# `-At '2,50'` (past the ~15 s gate) does not.
#
# AND WHY THE ENGINE-SIDE WATCHDOG CANNOT BE THE ONLY ANSWER: -VoxelMenuWatchdog
# is a FRONT-END mechanism, and the failure is precisely that the front end is
# already gone. A watchdog owned by the thing that has torn down is silent in
# exactly the case it exists for. Hence a bound out here, where nothing can
# tear down underneath it.
$process = Start-Process -FilePath $EditorExe -ArgumentList $argList -PassThru -NoNewWindow
if (-not $process.WaitForExit($TimeoutSec * 1000)) {
    Write-Warning ("TIMEOUT: the editor is still alive after {0}s and is being killed. " -f $TimeoutSec)
    Write-Warning ("  It has most likely FINISHED its work and not exited -- check the shot list and the log " +
                   "before assuming it was stuck. A shot time after hand-off does this (see the note above).")
    try { Stop-Process -Id $process.Id -Force -ErrorAction Stop } catch { Write-Warning "  kill failed: $_" }
    $null = $process.WaitForExit(15000)
}
Write-Host "editor exited with code $($process.ExitCode)"

# --- What the front end actually decided --------------------------------------
#
# Printed BEFORE the file list, because it is what tells you whether the file
# list is worth reading. "VoxelFrontEnd: suppressed (...)" means the picture is
# of an empty world, not of a menu.
if (Test-Path $LogPath) {
    $verdicts = Select-String -Path $LogPath -Pattern 'VoxelFrontEnd:|VoxelLoadGate:|LoadScreen:' -ErrorAction SilentlyContinue
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
    # THE SIZE IS MEASURED, NOT ASSUMED. The banner above echoes the parameter;
    # this reads the pixels. A capture whose image is not the requested size is
    # a picture of a resolution nobody asked for, and every measurement taken
    # off it -- an ADR-0011 shell width above all -- is attributed to the wrong
    # screen. voxel-run-flight-leg.ps1 grew the same check on 2026-08-25 after
    # exactly this failure, and this script did not have it.
    $mismatch = $false
    try { Add-Type -AssemblyName System.Drawing -ErrorAction Stop } catch { }
    $new | ForEach-Object {
        $size = ''
        try {
            $img = [System.Drawing.Image]::FromFile($_.FullName)
            $size = " -- $($img.Width)x$($img.Height)"
            if ($img.Width -ne $Width -or $img.Height -ne $Height) { $mismatch = $true }
            $img.Dispose()
        } catch { }
        Write-Host ("  " + $_.FullName + $size)
    }
    if ($mismatch) {
        Write-Warning ("RESOLUTION MISMATCH: ${Width}x${Height} was requested and the image(s) above are not that size.")
        Write-Warning ("  Quote the size printed above, never the request. Borderless fullscreen ignores -ResX/-ResY;")
        Write-Warning ("  a windowed request larger than the desktop work area is shrunk to fit it.")
    }
} elseif ($Shot -ne 'GateSweep') {
    Write-Warning "No new screenshot appeared in $ShotDir. Check the log above."
}
