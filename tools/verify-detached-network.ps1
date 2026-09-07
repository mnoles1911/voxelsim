[CmdletBinding()]
param(
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [int]$Port = 17879,
    [ValidateSet('nullrhi','dx12')][string]$Render = 'dx12',
    [int]$TimeoutSeconds = 240
)
$ErrorActionPreference = 'Stop'
$Workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Project = Join-Path $Workspace 'ue-project\VoxelEarth.uproject'
if (!(Test-Path -LiteralPath $Editor)) { throw "Editor missing: $Editor" }
$Busy = @(Get-Process -Name 'UnrealEditor','UnrealEditor-Cmd','UnrealBuildTool','cl','link','ShaderCompileWorker' -ErrorAction SilentlyContinue)
if ($Busy.Count) { throw ('Editor/compiler already running; leave it untouched and retry when idle: ' + (($Busy | ForEach-Object { "$($_.ProcessName):$($_.Id)" }) -join ', ')) }
if (Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue) { throw "UDP port $Port is already in use" }
$RunName = 'detached-net-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)
$RunDir = Join-Path $Workspace "Saved\Tests\$RunName"
New-Item -ItemType Directory -Path $RunDir | Out-Null
$Seed = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$SavePath = Join-Path $Workspace "ue-project\Saved\VoxelWorlds\$Seed.vxlog"
if (Test-Path -LiteralPath $SavePath) { throw "Refusing an existing seed save: $SavePath" }
$Logs = @{ server=(Join-Path $RunDir 'server.log'); client1=(Join-Path $RunDir 'client1.log'); client2=(Join-Path $RunDir 'client2.log') }
$Owned = [Collections.Generic.List[System.Diagnostics.Process]]::new()
$Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
function Read-Log([string]$Path) {
    if (Test-Path -LiteralPath $Path) { return (Get-Content -LiteralPath $Path -Raw) }
    return ''
}
function Wait-Log([string]$Path,[string]$Pattern) {
    while ([DateTime]::UtcNow -lt $Deadline) {
        $Text = Read-Log $Path
        if ($Text -match $Pattern) { return }
        foreach ($P in $Owned) { $P.Refresh(); if ($P.HasExited -and $P.ExitCode -ne 0) { throw "Process $($P.Id) failed ($($P.ExitCode)); logs: $RunDir" } }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for '$Pattern' in $Path"
}
function Launch([string]$Role) {
    $Url = if ($Role -eq 'server') { '/Engine/Maps/Entry' } else { "127.0.0.1:$Port" }
    $Args = @("`"$Project`"",$Url,'-unattended','-nosplash','-nosound','-log',"-abslog=`"$($Logs[$Role])`"",'-VoxelNoMenu','-VoxelSyntheticTerrain',"-VoxelSeed=$Seed",'-NoSteam')
    if ($Role -eq 'server') { $Args += @('-server',"-port=$Port",'-nullrhi','-VoxelObjectsNetVerify=server') }
    else { $Args += @('-game',"-$Render",'-sm6','-windowed','-ResX=640','-ResY=360','-VoxelObjectsNetVerify=client') }
    $P = Start-Process -FilePath $Editor -ArgumentList $Args -WorkingDirectory $Workspace -WindowStyle Hidden -PassThru
    $Owned.Add($P)
    Write-Host "Started $Role PID=$($P.Id) log=$($Logs[$Role])"
}
function State-At([string]$Text,[int]$Checkpoint,[string]$Id) {
    $Active = $false
    foreach ($Line in ($Text -split "`r?`n")) {
        if ($Line -match 'DetachedNetVerify checkpoint=(\d+)') { $Active = [int]$Matches[1] -eq $Checkpoint; continue }
        if ($Active -and $Line -match ('DetachedNetVerify id=' + [regex]::Escape($Id) + ' rev=(\d+) geom=(\d+) state=(\d+) actor=(\d+) retained=(\d+) pos=X=([-\d.]+) Y=([-\d.]+) Z=([-\d.]+)')) {
            return [pscustomobject]@{ Revision=[uint64]$Matches[1]; Geometry=[uint64]$Matches[2]; State=[int]$Matches[3]; Actor=[int]$Matches[4]; Retained=[int]$Matches[5]; X=[double]$Matches[6]; Y=[double]$Matches[7]; Z=[double]$Matches[8] }
        }
    }
    throw "Missing ID $Id at checkpoint $Checkpoint"
}
try {
    Launch 'server'
    Wait-Log $Logs.server ('IpNetDriver.*listening on port ' + $Port)
    Launch 'client1'
    Wait-Log $Logs.server 'DetachedNetFixture spawn id='
    # The second client connects only after the object moved and was retained.
    Wait-Log $Logs.server 'DetachedNetVerify checkpoint=8'
    Launch 'client2'
    foreach ($Role in @('server','client1','client2')) { Wait-Log $Logs[$Role] 'DetachedNetVerify completed role=' }
    foreach ($P in $Owned) {
        $Remaining = [Math]::Max(1,[int]($Deadline-[DateTime]::UtcNow).TotalMilliseconds)
        if (!$P.WaitForExit($Remaining)) { throw "Process $($P.Id) did not exit normally" }
        if ($P.ExitCode -ne 0) { throw "Process $($P.Id) exit code $($P.ExitCode)" }
    }
    $Text = @{};foreach ($Role in $Logs.Keys) { $Text[$Role]=Read-Log $Logs[$Role] }
    if ($Text.server -notmatch 'DetachedNetFixture spawn id=([A-Fa-f0-9-]+) cells=9 pos=X=([-\d.]+) Y=([-\d.]+) Z=([-\d.]+)') { throw 'Missing authoritative fixture spawn' }
    $Id=$Matches[1];$InitialX=[double]$Matches[2];$InitialY=[double]$Matches[3];$InitialZ=[double]$Matches[4]
    $Expected=State-At $Text.server 8 $Id
    if ([Math]::Abs($Expected.X-$InitialX-100) -gt .1) { throw 'Server fixture did not move 100 UU' }
    $Results=@{}
    foreach ($Role in @('server','client1','client2')) {
        if ($Text[$Role] -match 'DetachedNet[^\r\n]*(rejected|failed|exceeds)|Fatal error:') { throw "$Role reported network/engine failure" }
        $Before=State-At $Text[$Role] 8 $Id;$After=State-At $Text[$Role] 70 $Id
        if ($Before.Actor -ne 1 -or $Before.Retained -ne 1 -or $Before.Geometry -ne $Expected.Geometry) { throw "$Role did not receive retained geometry" }
        if ([Math]::Abs($Before.X-$Expected.X) -gt .1 -or [Math]::Abs($Before.Y-$Expected.Y) -gt .1 -or [Math]::Abs($Before.Z-$Expected.Z) -gt .1) { throw "$Role transform differs from server" }
        if ($After.State -ne 4 -or $After.Actor -ne 0) { throw "$Role did not receive deletion tombstone" }
        $Results[$Role]=@{before=$Before;after=$After}
    }
    if ($Text.client1 -notmatch ('DetachedNet installed id='+[regex]::Escape($Id)+' pos=X=([-\d.]+) Y=([-\d.]+) Z=([-\d.]+)')) { throw 'Early client never installed fixture' }
    if ([Math]::Abs([double]$Matches[1]-$InitialX) -gt .1) { throw 'Early client joined too late to verify the movement update' }
    $Report=@{passed=$true;id=$Id;seed=$Seed;port=$Port;render=$Render;terrain='synthetic isolated transport fixture';logs=$Logs;states=$Results;isolatedSave=$SavePath}
    $Report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $RunDir 'result.json')
    Write-Host "PASS: authoritative identity, motion, retention, late join and deletion agree. Results: $RunDir"
}
finally {
    # Only processes started by this invocation are eligible for watchdog cleanup.
    foreach ($P in $Owned) { $P.Refresh();if (!$P.HasExited) { Stop-Process -Id $P.Id -Force } }
}
