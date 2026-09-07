<#
.SYNOPSIS
Open the editor the way the owner means it: wait for a quiet box, launch UnrealEditor
on VoxelEarth.uproject, and land in Play In Editor on the title screen.

.DESCRIPTION
"Open the editor for me" (2026-09-07) turned out to mean "put me in the game", and a
bare UnrealEditor launch cannot do that: it opens an Untitled level, and the voxel
world only exists once a game world begins play (UVoxelWorldSubsystem refuses editor
worlds by design). So this script passes -VoxelAutoPIE, which
ue-project/Content/Python/init_unreal.py turns into an EditorRequestBeginPlay a few
seconds after the level editor is up. The PIE session is an ordinary interactive
launch as far as VoxelFrontEndPolicy is concerned, so it boots to the title screen
and NEW GAME/CONTINUE go through the front end like any player's session.

It waits for the box first, with the same busy list tools/voxel-build.ps1 refuses on
(plus ShaderCompileWorker), because a Codex commandlet or a build in flight makes the
first PIE session crawl and a link in flight can leave the editor holding a stale
DLL. It refuses to open a SECOND interactive editor on this project: two editors on
one checkout fight over Saved/ and the DDC, and the owner reads a second window as a
collision.

.PARAMETER NoAutoPIE
Open the editor without starting a play session.

.PARAMETER NoWait
Skip the idle wait (the launch still refuses if an editor on this project is open).

.PARAMETER WaitTimeoutSec
Give up waiting for a quiet box after this long. Default one hour.

.PARAMETER QuietSec
The box must be idle for this many consecutive seconds before the launch. Default 20,
matching the regen chains' Wait-Idle.

.EXAMPLE
pwsh tools/voxel-editor.ps1
.EXAMPLE
pwsh tools/voxel-editor.ps1 -NoAutoPIE -NoWait
#>
[CmdletBinding()]
param(
    [switch]$NoAutoPIE,
    [switch]$NoWait,
    [int]$WaitTimeoutSec = 3600,
    [int]$QuietSec = 20
)

$ErrorActionPreference = 'Stop'
$Uproject = 'D:\voxelsim\ue-project\VoxelEarth.uproject'
$Editor   = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'

if (-not (Test-Path $Editor)) { throw "UnrealEditor not found at $Editor (the dev box runs UE 5.8 at D:\UE_5.8)." }
if (-not (Test-Path $Uproject)) { throw "Project not found at $Uproject." }

# One interactive editor per checkout. Match on the project path so Codex's
# scratch-copy and worktree editors (different .uproject paths) do not count.
$existing = Get-CimInstance Win32_Process -Filter "Name='UnrealEditor.exe'" |
    Where-Object { $_.CommandLine -like "*$Uproject*" }
if ($existing) {
    foreach ($p in $existing) {
        Write-Host ("An editor on this project is already open: PID {0}, started {1}. Not launching a second." -f $p.ProcessId, $p.CreationDate)
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
if (-not $NoAutoPIE) { $argList += '-VoxelAutoPIE' }

$proc = Start-Process -FilePath $Editor -ArgumentList $argList -PassThru
Write-Host ("{0} UnrealEditor PID {1} launched: {2} {3}" -f (Get-Date -Format HH:mm:ss), $proc.Id, $Editor, ($argList -join ' '))
if (-not $NoAutoPIE) {
    Write-Host "PIE starts by itself a few seconds after the level editor appears (VoxelAutoPIE lines in Saved/Logs/VoxelEarth.log)."
}
