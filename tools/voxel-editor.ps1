<#
.SYNOPSIS
Open the game the way the owner means it: wait for a quiet box, then launch
VoxelEarth as a GAME-ONLY window (the title screen, then the world), with the full
editor and Play In Editor as an opt-in mode.

.DESCRIPTION
"Open the editor for me" (2026-09-07) turned out to mean "put me in the game". Two
things a bare UnrealEditor launch cannot do: it opens an Untitled level (the voxel
world only exists once a game world begins play -- UVoxelWorldSubsystem refuses
editor worlds by design), and even in PIE the editor host makes the game
unplayable on this box: the owner reported heavy input lag with a high in-game
FPS, and the log showed why -- roughly twice a second the game thread BLOCKS for
exactly 333 ms (GGameThreadWaitTime, frameMs=333.33) while the other frames run
at 100+ fps. That wait belongs to the editor host, not the game; the same build
under -game does not have it, which is why every capture and playtest leg runs
-game and why the owner remembers the game-only session running better.

So the DEFAULT here is -Mode Game: UnrealEditor.exe <project> -game, the owner's
own GameUserSettings for resolution and window mode, no editor tools. It is an
ordinary interactive launch as far as VoxelFrontEndPolicy is concerned, so it
boots to the title screen and NEW GAME/CONTINUE go through the front end like any
player's session, spawn rule included.

-Mode Editor is the old behaviour: the full editor, plus -VoxelAutoPIE, which
ue-project/Content/Python/init_unreal.py turns into an EditorRequestBeginPlay a few
seconds after the level editor is up. Use it when the editor tools are the point.

It waits for the box first, with the same busy list tools/voxel-build.ps1 refuses on
(plus ShaderCompileWorker), because a Codex commandlet or a build in flight makes the
first minutes crawl and a link in flight can leave the process holding a stale DLL.
It refuses to open a SECOND instance of the same mode on this project: two of them
fight over Saved/ and the DDC, and the owner reads a second window as a collision.

.PARAMETER Mode
Game (default): game-only window. Editor: full editor, auto-PIE.

.PARAMETER NoAutoPIE
Editor mode only: open the editor without starting a play session.

.PARAMETER NoWait
Skip the idle wait (the launch still refuses a second instance of the same mode).

.PARAMETER WaitTimeoutSec
Give up waiting for a quiet box after this long. Default one hour.

.PARAMETER QuietSec
The box must be idle for this many consecutive seconds before the launch. Default 20,
matching the regen chains' Wait-Idle.

.PARAMETER ExtraArgs
Extra command-line switches for the process, e.g. '-VoxelSpawnAt=-65102,-51084'.

.EXAMPLE
pwsh tools/voxel-editor.ps1
.EXAMPLE
pwsh tools/voxel-editor.ps1 -Mode Editor
.EXAMPLE
pwsh tools/voxel-editor.ps1 -NoWait -ExtraArgs '-VoxelSpawnAt=-65102,-51084'
#>
[CmdletBinding()]
param(
    [ValidateSet('Game', 'Editor')]
    [string]$Mode = 'Game',
    [switch]$NoAutoPIE,
    [switch]$NoWait,
    [int]$WaitTimeoutSec = 3600,
    [int]$QuietSec = 20,
    # ONE STRING, space-separated, not an array: through `powershell -File` an
    # array parameter arrives as a single comma-joined token, and
    # -VoxelSpawnAt=X,Y carries a comma of its own, so an array could never be
    # split back apart safely. Start-Process joins its ArgumentList with
    # spaces, so a string that already contains spaces passes straight through.
    [string]$ExtraArgs = ''
)

$ErrorActionPreference = 'Stop'
$Uproject = 'D:\voxelsim\ue-project\VoxelEarth.uproject'
$Editor   = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'

if (-not (Test-Path $Editor)) { throw "UnrealEditor not found at $Editor (the dev box runs UE 5.8 at D:\UE_5.8)." }
if (-not (Test-Path $Uproject)) { throw "Project not found at $Uproject." }

# One interactive instance PER MODE per checkout. Match on the project path so
# Codex's scratch-copy and worktree editors (different .uproject paths) do not
# count; a -game window and a full editor may coexist (that is what the
# editor's own Standalone Game does), but two of the same kind may not.
$wantGame = ($Mode -eq 'Game')
$existing = Get-CimInstance Win32_Process -Filter "Name='UnrealEditor.exe'" |
    Where-Object { $_.CommandLine -like "*$Uproject*" -and (($_.CommandLine -match '(^|\s)-game(\s|$)') -eq $wantGame) }
if ($existing) {
    foreach ($p in $existing) {
        Write-Host ("A {0} instance on this project is already open: PID {1}, started {2}. Not launching a second." -f $Mode, $p.ProcessId, $p.CreationDate)
    }
    exit 0
}

function Get-BusyProcesses {
    # voxel-build.ps1's list, plus the shader workers a running editor or commandlet spawns.
    @(Get-Process UnrealEditor-Cmd, UnrealEditor, ShaderCompileWorker, cl, link, UnrealBuildTool, MSBuild, dotnet -ErrorAction SilentlyContinue)
}

if (-not $NoWait) {
    $deadline   = (Get-Date).AddSeconds($WaitTimeoutSec)
    $quietSince = $null
    $lastNames  = ''
    while ($true) {
        $busy = Get-BusyProcesses
        if ($busy.Count -eq 0) {
            if ($null -eq $quietSince) { $quietSince = Get-Date }
            if (((Get-Date) - $quietSince).TotalSeconds -ge $QuietSec) { break }
        } else {
            $quietSince = $null
            $names = ($busy | Select-Object -ExpandProperty ProcessName -Unique) -join ', '
            if ($names -ne $lastNames) {
                Write-Host ("{0} box busy: {1} -- waiting for {2} s of quiet (deadline {3})" -f (Get-Date -Format HH:mm:ss), $names, $QuietSec, $deadline.ToString('HH:mm:ss'))
                $lastNames = $names
            }
        }
        if ((Get-Date) -gt $deadline) { throw "Gave up waiting for a quiet box after $WaitTimeoutSec s (last busy: $lastNames)." }
        Start-Sleep -Seconds 5
    }
}

$argList = @("`"$Uproject`"")
if ($wantGame) {
    # Game-only window. Resolution and window mode come from the owner's own
    # GameUserSettings.ini; nothing here forces them.
    #
    # t.IdleWhenNotForeground=1: when the owner tabs away, the game stops
    # taking CPU and GPU. Without it a -game window keeps rendering flat out
    # in the background (title screen: ~4 cores and 75% of the GPU on
    # 2026-09-07), which made the desktop, and tabbing back in, crawl.
    # COMMAND LINE ONLY, never DefaultEngine.ini: FEngineLoop::ShouldUseIdleMode
    # applies it to any -game process without focus, unattended included, so a
    # project-wide setting would freeze every headless capture and Codex leg
    # the moment its window was not in front.
    $argList += '-game'
    $argList += '-dpcvars=t.IdleWhenNotForeground=1'
} elseif (-not $NoAutoPIE) {
    $argList += '-VoxelAutoPIE'
}
if ($ExtraArgs.Trim()) { $argList += $ExtraArgs.Trim() }

$proc = Start-Process -FilePath $Editor -ArgumentList $argList -PassThru
Write-Host ("{0} UnrealEditor PID {1} launched ({2}): {3} {4}" -f (Get-Date -Format HH:mm:ss), $proc.Id, $Mode, $Editor, ($argList -join ' '))
if ($wantGame) {
    Write-Host "Game-only window: title screen, then NEW GAME / CONTINUE. Log: Saved/Logs/VoxelEarth.log."
} elseif (-not $NoAutoPIE) {
    Write-Host "PIE starts by itself a few seconds after the level editor appears (VoxelAutoPIE lines in Saved/Logs/VoxelEarth.log)."
}
