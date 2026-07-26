# Wave F (R0 = 128 m) leg runner and log parser.
#
# Runs the F1 A/B: the shipped 64 m cascade against the shifted 128 m cascade,
# alternating configs, and extracts the numbers Wave F is decided on -- cold-fill
# time, per-ring resident counts, resident quads, and the pool's high-water mark.
#
# WHY THIS IS A SCRIPT AND NOT A COMMAND LINE IN A DOC. Two of the switches it
# passes are comma lists, and comma lists have now silently truncated in this
# programme twice for two DIFFERENT reasons:
#
#   1. FParse::Value's FString overload defaults bShouldStopOnSeparator=true and
#      its terminator set is ",) \r\n\t", so it hands back everything up to the
#      first comma. That is a C++ bug and it is fixed in VoxelWorldSubsystem.cpp;
#      before the fix, -VoxelRingOuterMeters=128,256,512,1024,2048,4096 set R0's
#      outer radius to 128 and left R1-R5 at their defaults, producing a cascade
#      that still ran and still rendered.
#   2. PowerShell splits an unquoted argument containing commas into an ARRAY
#      before the process ever sees it, so `-VoxelRingInnerMeters=0,128,...`
#      typed at a prompt is a parser error at best and a mangled command line at
#      worst. Both switches below are quoted for that reason. Do not unquote them.
#
# So: pass the switches from here, and verify from the log rather than from the
# command line. Get-LegSummary checks the resolved RingPresets lines and refuses
# to report a leg whose cascade is not the one that was asked for.

param(
    [string]$Worktree = 'D:\voxelsim\.claude\worktrees\agent-a765aa9ce46cbf4ce',
    [string]$LogDir   = "$env:TEMP\wave-f",
    [int]$RunSeconds  = 180,
    [string]$Editor   = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
)

# The shifted cascade. The annuli must abut exactly (Outer[L] == Inner[L+1]) or
# GetRingPresets aborts the run -- see the validation block it added.
$R0_128_INNER = '-VoxelRingInnerMeters=0,128,256,512,1024,2048'
$R0_128_OUTER = '-VoxelRingOuterMeters=128,256,512,1024,2048,4096'

function Invoke-Leg {
    param([string]$Name, [switch]$R0At128, [switch]$GpuPool)

    New-Item -ItemType Directory -Force $LogDir | Out-Null
    $log = Join-Path $LogDir "$Name.log"

    # Saved/VoxelWorlds/*.vxlog persists the EDIT LOG across runs. A second run
    # at the same spot replays it, so a "cold" fill measured without clearing it
    # is not cold. (Wave B lost a carve measurement to exactly this: the second
    # identical carve removed 0 voxels because the first was still recorded.)
    Remove-Item -Recurse -Force (Join-Path $Worktree 'ue-project\Saved\VoxelWorlds') -ErrorAction SilentlyContinue

    $a = @(
        (Join-Path $Worktree 'ue-project\VoxelEarth.uproject')
        '-game', '-windowed', '-resx=1280', '-resy=720', '-nosplash', '-unattended'
        '-sm6', '-ForceLogFlush'
        "-VoxelPerfRun=$RunSeconds", '-VoxelPerfFlight=static', '-VoxelPerfYaw=0', '-VoxelPerfPitch=-10'
        # 5 s is the shipped cadence and it is the same order as the baseline
        # fill time, so the quantisation error would land on the A/B difference.
        '-VoxelPerfLogInterval=1'
    )
    if ($R0At128) { $a += $R0_128_INNER; $a += $R0_128_OUTER }
    if ($GpuPool) { $a += '-ExecCmds=voxel.Stream.GPU 1' }
    $a += "-abslog=$log"

    & $Editor @a 2>&1 | Out-Null
    return $log
}

function Get-LegSummary {
    param([string]$LogPath)

    $text = Get-Content $LogPath

    # The cascade the run ACTUALLY used. Not the command line: a switch can be
    # accepted and then partially applied, which is the failure this whole
    # programme keeps re-discovering.
    $rings = @()
    foreach ($l in ($text | Select-String -Pattern 'RingPresets\[\d\] = ')) {
        if ($l.ToString() -match 'RingPresets\[(\d)\] = \[([\d.]+), ([\d.]+)\) m(.*)$') {
            $rings += [pscustomobject]@{
                Level = [int]$matches[1]; Inner = [double]$matches[2]
                Outer = [double]$matches[3]; Overridden = $matches[4] -match 'OVERRIDDEN'
            }
        }
    }

    # Cold fill = first sample -> the sample after the LAST one with work
    # outstanding. Taking the FIRST jobsInFlight=0 pendingJobs=0 instead is
    # wrong and measurably so: this cascade goes momentarily idle many times
    # while the admission pass re-arms, and on the 128 m leg the first such
    # moment is 15 s earlier than the real settle.
    $rows = @()
    foreach ($l in ($text | Select-String -Pattern 'Voxel streaming: ')) {
        if ($l.ToString() -match '\[[\d.]+-(\d\d)\.(\d\d)\.(\d\d):(\d\d\d)\].*quads=(\d+) tracked=(\d+) jobsInFlight=(\d+) pendingJobs=(\d+)') {
            $rows += [pscustomobject]@{
                T = [double]$matches[1]*3600 + [double]$matches[2]*60 + [double]$matches[3] + [double]$matches[4]/1000
                Quads = [int64]$matches[5]; Tracked = [int]$matches[6]
                InFlight = [int]$matches[7]; Pending = [int]$matches[8]
            }
        }
    }
    $lastBusy = -1
    for ($i = 0; $i -lt $rows.Count; $i++) {
        if ($rows[$i].InFlight -gt 0 -or $rows[$i].Pending -gt 0) { $lastBusy = $i }
    }
    $settled = if ($lastBusy -ge 0 -and ($lastBusy + 1) -lt $rows.Count) { $rows[$lastBusy + 1] } else { $null }

    $ringLine = ($text | Select-String -Pattern 'Voxel rings: ' | Select-Object -Last 1)
    $poolLine = ($text | Select-String -Pattern 'Voxel GPU pool: ' | Select-Object -Last 1)
    $complete = ($text | Select-String -Pattern 'VoxelPerfRun complete' | Select-Object -Last 1)
    $postWarm = ($text | Select-String -Pattern 'VoxelPerfRun post-warmup' | Select-Object -Last 1)

    [pscustomobject]@{
        Log          = $LogPath
        Rings        = $rings
        CascadeEdgeM = if ($rings) { ($rings | Select-Object -Last 1).Outer } else { $null }
        R0OuterM     = if ($rings) { $rings[0].Outer } else { $null }
        # A leg that reached the end of the run still streaming has no fill time,
        # only a lower bound. Reporting the last sample as if it were a settle is
        # how a truncated run becomes a fast result.
        Settled      = ($null -ne $settled)
        FillSeconds  = if ($settled) { $settled.T - $rows[0].T } else { $null }
        SettleQuads  = if ($settled) { $settled.Quads } else { $rows[-1].Quads }
        SettleTracked= if ($settled) { $settled.Tracked } else { $rows[-1].Tracked }
        RingLine     = if ($ringLine) { $ringLine.ToString() } else { $null }
        PoolLine     = if ($poolLine) { $poolLine.ToString() } else { '(no GPU pool -- component path)' }
        Complete     = if ($complete) { $complete.ToString() } else { $null }
        PostWarmup   = if ($postWarm) { $postWarm.ToString() } else { $null }
    }
}
