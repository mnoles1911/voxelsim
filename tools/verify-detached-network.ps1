[CmdletBinding()]
param(
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [string]$Python = '',
    [int]$Port = 17879,
    [ValidateSet('nullrhi','dx12')][string]$Render = 'dx12',
    [int]$TimeoutSeconds = 240,
    [switch]$AllowOtherProjectEditors
)
$ErrorActionPreference = 'Stop'
$Workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Project = Join-Path $Workspace 'ue-project\VoxelEarth.uproject'
if (!(Test-Path -LiteralPath $Editor)) { throw "Editor missing: $Editor" }
$TransportCreator = Join-Path $PSScriptRoot 'create-session-transport.py'
if (!(Test-Path -LiteralPath $TransportCreator)) { throw 'Secure transport generator missing; merge the session transport implementation before running this harness.' }
if (!$Python) {
    $PythonCommand = Get-Command python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($PythonCommand) { $Python = $PythonCommand.Source }
}
if (!$Python -or !(Test-Path -LiteralPath $Python -PathType Leaf)) { throw 'Python executable unavailable; pass -Python <absolute-path-to-python.exe>.' }
$Busy = @(Get-Process -Name 'UnrealEditor','UnrealEditor-Cmd','UnrealBuildTool','cl','link','ShaderCompileWorker' -ErrorAction SilentlyContinue)
if ($AllowOtherProjectEditors) {
    # Only an explicitly identified DIFFERENT project may coexist. Unknown
    # command lines and every compiler remain blocking. This is functional
    # transport validation, not a valid concurrent performance measurement.
    $Busy = @($Busy | Where-Object {
        if ($_.ProcessName -notin @('UnrealEditor','UnrealEditor-Cmd')) { return $true }
        $Command = (Get-CimInstance Win32_Process -Filter "ProcessId = $($_.Id)").CommandLine
        if ($Command -match '(?i)(?:"([^\"]+\.uproject)"|([^\s"]+\.uproject))') {
            $OtherProject = if ($Matches[1]) { $Matches[1] } else { $Matches[2] }
            return [IO.Path]::GetFullPath($OtherProject) -eq $Project
        }
        return $true
    })
}
if ($Busy.Count) { throw ('Editor/compiler already running; leave it untouched and retry when idle: ' + (($Busy | ForEach-Object { "$($_.ProcessName):$($_.Id)" }) -join ', ')) }
$BuildTools = @(Get-CimInstance Win32_Process -Filter "Name = 'dotnet.exe'" | Where-Object { $_.CommandLine -match 'UnrealBuildTool' })
if ($BuildTools.Count) { throw 'UnrealBuildTool is active, possibly between compiler actions. Leave that build untouched and retry when idle.' }
if (Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue) { throw "UDP port $Port is already in use" }
$RunName = 'detached-net-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)
$RunDir = Join-Path $Workspace "Saved\Tests\$RunName"
New-Item -ItemType Directory -Path $RunDir | Out-Null
$PrivateDir = Join-Path $RunDir 'private'
# The generator creates a NEW directory and writes secrets only to files.
# Never print invite JSON or place its key in a URL or process argument.
& $Python $TransportCreator $PrivateDir --clients 2
if ($LASTEXITCODE -ne 0) { throw 'Private transport invite generation failed; refusing to launch without encryption.' }
$ServerKeys = Join-Path $PrivateDir 'server.json'
if (!(Test-Path -LiteralPath $ServerKeys -PathType Leaf)) { throw 'Transport generator did not produce the server key index.' }
$ClientTransport = @{}
foreach ($Number in 1..2) {
    $InvitePath = Join-Path $PrivateDir "invite-$Number.json"
    $Invite = Get-Content -LiteralPath $InvitePath -Raw | ConvertFrom-Json
    if ($Invite.version -ne 1 -or $Invite.id -cnotmatch '^[a-f0-9]{32}$') { throw "Transport invite $Number has invalid public metadata." }
    $ClientTransport["client$Number"] = @{ Path=$InvitePath; Id=[string]$Invite.id; Profile="$RunName-client-$Number" }
    $Invite = $null
}
if ($ClientTransport.client1.Id -eq $ClientTransport.client2.Id) { throw 'Distinct client transport invites are required.' }
$GateFile = Join-Path $RunDir 'clients-ready.marker'
$EarlyFile = $GateFile + '.early'
$CompleteFile = $GateFile + '.complete'
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
        $Exited = 0
        foreach ($P in $Owned) {
            $P.Refresh()
            if ($P.HasExited) {
                ++$Exited
                if ($P.ExitCode -ne 0) { throw "Process $($P.Id) failed ($($P.ExitCode)); logs: $RunDir" }
            }
        }
        if ($Owned.Count -gt 0 -and $Exited -eq $Owned.Count) { throw "All owned processes exited without '$Pattern'; logs: $RunDir" }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for '$Pattern' in $Path"
}
function Launch([string]$Role) {
    $Url = if ($Role -eq 'server') { '/Engine/Maps/Entry' } else { "127.0.0.1:$Port`?EncryptionToken=$($ClientTransport[$Role].Id)" }
    $Args = @("`"$Project`"",$Url,'-unattended','-nosplash','-nosound','-Multiprocess','-log',"-abslog=`"$($Logs[$Role])`"",'-VoxelNoMenu','-VoxelSyntheticTerrain',"-VoxelSeed=$Seed",'-NoSteam',"-VoxelObjectsNetGateFile=`"$GateFile`"")
    if ($Role -eq 'server') { $Args += @('-server',"-port=$Port",'-nullrhi','-VoxelObjectsNetVerify=server',"-VoxelTransportKeys=`"$ServerKeys`"") }
    else { $Args += @('-game',"-$Render",'-sm6','-windowed','-ResX=640','-ResY=360','-VoxelObjectsNetVerify=client',"-VoxelTransportInvite=`"$($ClientTransport[$Role].Path)`"","-VoxelPlayerProfile=$($ClientTransport[$Role].Profile)") }
    $P = Start-Process -FilePath $Editor -ArgumentList $Args -WorkingDirectory $Workspace -WindowStyle Hidden -PassThru
    $Owned.Add($P)
    Write-Host "Started $Role PID=$($P.Id) log=$($Logs[$Role])"
}
function State-At([string]$Text,[int]$Checkpoint,[string]$Id,[switch]$AllowMissing) {
    $Active = $false
    $Found = $null
    foreach ($Line in ($Text -split "`r?`n")) {
        if ($Line -match 'DetachedNetVerify checkpoint=(\d+)') {
            if ($Active -and $null -ne $Found) { return $Found }
            $Active = [int]$Matches[1] -eq $Checkpoint; continue
        }
        if ($Active -and $Line -match ('DetachedNetVerify id=' + [regex]::Escape($Id) + ' rev=(\d+) geom=(\d+) state=(\d+) actor=(\d+) retained=(\d+) pos=X=([-\d.]+) Y=([-\d.]+) Z=([-\d.]+)')) {
            $Found = [pscustomobject]@{ Revision=[uint64]$Matches[1]; Geometry=[uint64]$Matches[2]; State=[int]$Matches[3]; Actor=[int]$Matches[4]; Retained=[int]$Matches[5]; X=[double]$Matches[6]; Y=[double]$Matches[7]; Z=[double]$Matches[8]; VisualError=$null }
        }
        if ($Active -and $null -ne $Found -and $Line -match ('DetachedNetVerify visual id=' + [regex]::Escape($Id) + ' errorCm=([\d.]+)')) {
            $Found.VisualError = [double]$Matches[1]
            return $Found
        }
    }
    if ($null -ne $Found) { return $Found }
    if ($AllowMissing) { return $null }
    throw "Missing ID $Id at checkpoint $Checkpoint"
}
function Wait-State([string]$Role,[int]$Checkpoint,[string]$Id) {
    while ([DateTime]::UtcNow -lt $Deadline) {
        $Text = Read-Log $Logs[$Role]
        if ($Text -match 'DetachedNet[^\r\n]*(rejected|failed|exceeds)|Fatal error:') { throw "$Role reported network/engine failure" }
        $State = State-At $Text $Checkpoint $Id -AllowMissing
        if ($null -ne $State -and ($Checkpoint -ne 8 -or $null -ne $State.VisualError)) { return $State }
        $Exited = 0
        foreach ($P in $Owned) {
            $P.Refresh()
            if ($P.HasExited) {
                ++$Exited
                if ($P.ExitCode -ne 0) { throw "Process $($P.Id) failed ($($P.ExitCode)); logs: $RunDir" }
            }
        }
        if ($Owned.Count -gt 0 -and $Exited -eq $Owned.Count) { throw "All owned processes exited without $Role checkpoint $Checkpoint; logs: $RunDir" }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for $Role checkpoint $Checkpoint for $Id; logs: $RunDir"
}
try {
    Launch 'server'
    Wait-Log $Logs.server ('IpNetDriver.*listening on port ' + $Port)
    Launch 'client1'
    Wait-Log $Logs.server 'DetachedNetFixture spawn id='
    $ServerText = Read-Log $Logs.server
    if ($ServerText -notmatch 'DetachedNetFixture spawn id=([A-Fa-f0-9-]+) cells=9 pos=X=([-\d.]+) Y=([-\d.]+) Z=([-\d.]+)') { throw 'Missing authoritative fixture spawn' }
    $Id=$Matches[1];$InitialX=[double]$Matches[2];$InitialY=[double]$Matches[3];$InitialZ=[double]$Matches[4]
    # Movement starts only after the early client actually installed the initial
    # authoritative geometry. Cold restoration must not race a two-second timer.
    $InstalledPattern = 'DetachedNet installed id='+[regex]::Escape($Id)+' pos=X=([-\d.]+) Y=([-\d.]+) Z=([-\d.]+)'
    Wait-Log $Logs.client1 $InstalledPattern
    $EarlyText = Read-Log $Logs.client1
    if ($EarlyText -notmatch $InstalledPattern) { throw 'Early client never installed fixture' }
    if ([Math]::Abs([double]$Matches[1]-$InitialX) -gt .1 -or [Math]::Abs([double]$Matches[2]-$InitialY) -gt .1 -or [Math]::Abs([double]$Matches[3]-$InitialZ) -gt .1) { throw 'Early client initial transform differs from server' }
    [IO.File]::WriteAllText($EarlyFile, "Early client installed initial fixture $Id")
    # The second client connects only after the object moved and was retained.
    Wait-Log $Logs.server 'DetachedNetVerify checkpoint=8'
    $ExpectedReady = Wait-State 'server' 8 $Id
    Launch 'client2'
    # The file requests a server action only AFTER the harness has observed
    # both real client replicas, including the cold late-join restoration.
    foreach ($Role in @('client1','client2')) {
        $Ready = Wait-State $Role 8 $Id
        if ($Ready.Actor -ne 1 -or $Ready.State -ne 0 -or $Ready.Retained -ne 1 -or $Ready.Geometry -ne $ExpectedReady.Geometry -or $Ready.VisualError -gt .1) { throw "$Role did not converge to live retained geometry before deletion" }
        if ([Math]::Abs($Ready.X-$ExpectedReady.X) -gt .1 -or [Math]::Abs($Ready.Y-$ExpectedReady.Y) -gt .1 -or [Math]::Abs($Ready.Z-$ExpectedReady.Z) -gt .1) { throw "$Role transform differs from server before deletion" }
    }
    [IO.File]::WriteAllText($GateFile, "Both clients observed retained fixture $Id")
    foreach ($Role in @('server','client1','client2')) {
        $Deleted = Wait-State $Role 70 $Id
        if ($Deleted.State -ne 4 -or $Deleted.Actor -ne 0) { throw "$Role did not observe the deletion tombstone" }
    }
    [IO.File]::WriteAllText($CompleteFile, "Both clients observed tombstone $Id")
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
        if ($Before.Actor -ne 1 -or $Before.State -ne 0 -or $Before.Retained -ne 1 -or $Before.Geometry -ne $Expected.Geometry) { throw "$Role did not receive live retained geometry" }
        $Visual = [regex]::Matches($Text[$Role], ('DetachedNetVerify visual id=' + [regex]::Escape($Id) + ' errorCm=([\d.]+)'))
        if (!$Visual.Count) { throw "$Role did not report its displayed actor pose" }
        foreach ($Sample in $Visual) { if ([double]$Sample.Groups[1].Value -gt .1) { throw "$Role displayed pose did not converge to authoritative state" } }
        if ([Math]::Abs($Before.X-$Expected.X) -gt .1 -or [Math]::Abs($Before.Y-$Expected.Y) -gt .1 -or [Math]::Abs($Before.Z-$Expected.Z) -gt .1) { throw "$Role transform differs from server" }
        if ($After.State -ne 4 -or $After.Actor -ne 0) { throw "$Role did not receive deletion tombstone" }
        $Results[$Role]=@{before=$Before;after=$After}
    }
    if ($Text.client1 -notmatch ('DetachedNet installed id='+[regex]::Escape($Id)+' pos=X=([-\d.]+) Y=([-\d.]+) Z=([-\d.]+)')) { throw 'Early client never installed fixture' }
    if ([Math]::Abs([double]$Matches[1]-$InitialX) -gt .1) { throw 'Early client joined too late to verify the movement update' }
    $Report=@{passed=$true;id=$Id;seed=$Seed;port=$Port;render=$Render;terrain='synthetic isolated transport fixture';logs=$Logs;states=$Results;isolatedSave=$SavePath;earlyBarrier=$EarlyFile;readyBarrier=$GateFile;completionBarrier=$CompleteFile}
    $Report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $RunDir 'result.json')
    Write-Host "PASS: authoritative identity, motion, retention, late join and deletion agree. Results: $RunDir"
}
finally {
    # Only processes started by this invocation are eligible for watchdog cleanup.
    foreach ($P in $Owned) { $P.Refresh();if (!$P.HasExited) { Stop-Process -Id $P.Id -Force } }
}
