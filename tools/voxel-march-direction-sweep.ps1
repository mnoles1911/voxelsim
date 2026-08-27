# Measure the ray marcher's GPU cost PER LOOK DIRECTION, A/B, in one command.
#
# ============================================================================
# WHY THIS EXISTS
# ============================================================================
#
# The requirement two builds are chasing is "cut marcher ray cost in EVERY
# direction". The evidence for or against that is a per-direction table, and
# until now that table was produced by hand: four ad-hoc legs
# (Saved/ZZ-pitch-90.log, ZZ-pitch0.log, ZZ-pitch-20.log, ZZ-pitch30.log) whose
# pose and capture were smuggled in as ONE comma-glued extra argument,
#
#     -VoxelPerfPitch=0,-VoxelExecCmds=ProfileGPU,-VoxelExecAfter=110
#
# which reaches the editor as a SINGLE argv token and only works because
# FParse::Value does a substring search of the whole command line. It has been
# retyped for every experiment since, and the retyping is where results go wrong.
#
# ============================================================================
# WHAT THE NUMBER IS, AND WHAT IT IS NOT
# ============================================================================
#
# THE NUMBER IS `VoxelMarch.March` FROM ProfileGPU. Nothing else. Three separate
# cvar docs in VoxelMarchRenderer.cpp say so in capitals, for the reason Aila &
# Laine and VoxelRT both published: iteration counts and wall time move in
# OPPOSITE directions often enough that a skipped-cells counter is not evidence
# of a saving.
#
# IT IS NOT `marchMs` FROM voxel.March.Stats. That is a running mean over every
# frame of the leg and it read 6.348 on the same leg where ProfileGPU read
# 7.178. March.Stats is used here for PROOF OF TRAFFIC and for the
# frames/emitFrames pair, never for the headline.
#
# IT IS NOT FRAME TIME. Frame time is printed alongside as context and is
# labelled as context.
#
# ============================================================================
# THE INSTRUMENT IS A SHADER PERMUTATION -- THE TRAP THAT BIT THE LAST SWEEP
# ============================================================================
#
# voxel.March.HoleStats is a PERMUTATION of the timed kernel ("off is FREE: no
# UAV is created or bound, no groupshared word exists, no atomic runs" --
# CVarVoxelMarchHoleStats). It is ALSO the switch that makes voxel.March.Stats
# print blkConsulted / blkSkipped / blkCellsAvoided, i.e. the proof that an arm
# engaged. So the leg that can prove engagement is NOT the leg that can be
# timed, and the archive already shows the two being mixed:
#
#     ZZ-pitch0   HoleStats 0   4.448 ms      <- the quoted baseline
#     BK-a0p0     HoleStats 1   4.548 ms      <- same arm, +2.2%
#     BL-a0p0     HoleStats 1   4.463 ms
#
# This script therefore OWNS voxel.March.HoleStats and refuses an arm string
# that sets it:
#
#   * TIMING legs run HoleStats 0 and are the only legs whose ms is quoted.
#   * ENGAGEMENT legs run HoleStats 1, at one pose, once per armed arm, and
#     exist only to print the arm's own counters. Their ms is never quoted.
#
# ============================================================================
# SOMETHING MOVES UNDERNEATH: RECORD THE BINARY
# ============================================================================
#
# Same pose, same spawn, same view=1552x873, same HoleStats state, eight hours
# apart:
#
#     BK-a0p0        2026-08-25 20:44   4.548 ms
#     CENSUS-pitch0  2026-08-26 02:05   5.695 ms      +25%
#
# Neither log records which build it ran. Every sweep here writes the mtimes of
# UnrealEditor-VoxelEarth.dll, UnrealEditor-VoxelEarthShaders.dll, voxelcore.lib
# and the shader build stamp into a manifest beside the logs, so a cross-run
# comparison can be refused rather than published.
#
# ============================================================================
# RUN-TO-RUN NOISE FLOOR, MEASURED
# ============================================================================
#
# BK and BL are the same six legs run twice, 30 minutes apart, same build:
#
#     pose       BK       BL      spread
#     a0 p-90    1.116    1.116    0.0%
#     a0 p0      4.548    4.463    1.9%
#     a0 p30     5.663    5.609    1.0%
#     a1 p-90    1.449    1.443    0.4%
#     a1 p0      5.934    5.903    0.5%
#     a1 p30     7.138    7.178    0.6%
#
# So a single A/B pair resolves ~2% at the horizon and no better. An arm whose
# claimed effect is under 2% needs -Repeats, not a louder assertion.
#
# ============================================================================
# USAGE
# ============================================================================
#
#   # dry run: print exactly what would be executed, launch nothing
#   tools\voxel-march-direction-sweep.ps1 -Prefix DIR1 -DryRun `
#       -ArmBName pyr -ArmBCvars "voxel.March.HeightPyramid 1" `
#       -ArmBProof "hpyr:.*consulted=[1-9]"
#
#   # the real thing
#   tools\voxel-march-direction-sweep.ps1 -Prefix DIR1 `
#       -ArmBName pyr -ArmBCvars "voxel.March.HeightPyramid 1" `
#       -ArmBProof "hpyr:.*consulted=[1-9]"
#
#   # control-only baseline reproduction (no B arm, baseline three poses)
#   tools\voxel-march-direction-sweep.ps1 -Prefix BASE -Poses '-90,0','0,0','30,0'
#
# Then:  bash tools/march-direction-summary.sh DIR1

param(
    # Log name prefix. Every leg is Saved/<Prefix>-<arm>-p<pitch>y<yaw>[-r<n>].log
    # and the manifest is Saved/<Prefix>-manifest.tsv.
    [Parameter(Mandatory=$true)][string]$Prefix,

    # ------------------------------------------------------------------
    # THE POSE SET, AND WHY IT IS SIX AND NOT THREE
    # ------------------------------------------------------------------
    # Each entry is "pitch,yaw" in degrees. -VoxelPerfFlight static pins BOTH
    # position and rotation, so a pose is exactly reproducible.
    #
    #   -90,0    STRAIGHT DOWN. The floor of the measurement: almost no empty
    #            space is crossed, ground is hit within metres. Every ratio in
    #            the output is against this pose, because the thesis under test
    #            is "cost tracks empty space crossed" and this is the pose with
    #            none of it. Yaw is not swept here: at pitch -90 yaw only rolls
    #            the image about the view axis, so the cone of directions
    #            sampled is identical.
    #
    #    0,0     THE HORIZON, and the pose the frame is actually spent in. This
    #            is the yaw the 4.450 ms baseline was taken at, so it stays in
    #            the set for continuity.
    #
    #    0,120   THE HORIZON, TWO MORE AZIMUTHS. A single yaw samples ONE
    #    0,240   direction across terrain that is not isotropic -- ridge lines,
    #            valley axes and the fine-tile seam all have orientation, and
    #            how far a ray travels through air before it hits anything is
    #            exactly what this sweep is measuring. Three yaws 120 deg apart
    #            give a mean AND a spread, and the spread is the falsifier: an
    #            arm whose effect is smaller than the azimuthal spread has not
    #            been shown to do anything at the horizon.
    #
    #   30,0     SKY. The pose the current mechanisms are aimed at, and the
    #            5.638 ms end of the baseline.
    #
    #   90,0     STRAIGHT UP. Included deliberately, and it is not a pose a
    #            player holds. It is the UPPER BOUND of the mechanism's own
    #            claim: above the chunk grid there is no acceleration structure
    #            and no Z bound at all, so a near-vertical ray walks the full
    #            4,198 m half-extent (~1,310 chunk steps) and returns
    #            TERM_CHUNK_CAP having produced nothing. Any mechanism that
    #            cuts empty-space traversal must show its LARGEST effect here.
    #            One that helps +30 and not +90 is doing something else, and
    #            that is worth knowing before the mechanism is described to the
    #            owner as an empty-space skip.
    #
    # Drop to '-90,0','0,0','30,0' for a fast baseline reproduction.
    [string[]]$Poses = @('-90,0', '0,0', '0,120', '0,240', '30,0', '90,0'),

    # ------------------------------------------------------------------
    # THE TWO ARMS
    # ------------------------------------------------------------------
    # Arm A defaults to the CONTROL: base cvars only, nothing added. Give arm B
    # the mechanism under test. Both arms get -BaseCvars plus the capture
    # plumbing, so they differ in exactly what -Arm?Cvars says and nothing else.
    [string]$ArmAName  = 'ctl',
    [string]$ArmACvars = '',
    [string]$ArmAProof = '',
    [string]$ArmBName  = '',
    [string]$ArmBCvars = '',
    [string]$ArmBProof = '',

    # PROOF OF TRAFFIC IS MANDATORY FOR AN ARMED ARM.
    # This project shipped eleven arms in one night that reported success and
    # did nothing. An arm that sets cvars must name a regular expression that
    # its OWN log line matches only when it carried traffic -- e.g.
    # "blkConsulted=[1-9]", not "armed=1", which a cvar reads back on a frame
    # the pass declined. -NoProofRequired suppresses the refusal and prints a
    # banner on every leg and in the manifest saying the sweep proved nothing.
    [switch]$NoProofRequired,

    # ------------------------------------------------------------------
    # THE SCENE. These are the baseline's values; changing one voids
    # comparison with the quoted baseline and the manifest records it.
    # ------------------------------------------------------------------
    # -84480,53760 IS FATAL: 38 km outside the baked fine tile set, every
    # elevation query answered with sea level, VoxelFineTileStreamer.cpp kills
    # an unattended run rather than measure a world that is not there.
    [string]$SpawnAt      = '-61440,-61440',
    [string]$BaseCvars    = 'voxel.Stream.CoverageVerify 1',
    [int]$Width           = 2560,
    [int]$Height          = 1440,
    [int]$PreflightSec    = 75,
    [int]$RunSec          = 45,
    [int]$LingerSec       = 10,
    [int]$LogIntervalSec  = 5,
    [int]$TimeoutSec      = 480,
    # Seconds from -ExecCmds (engine startup) to the ProfileGPU capture. 110 is
    # the baseline legs' value. It is NOT trusted: every leg checks the capture
    # actually landed between "STATIC pose pinned" and the start of the linger
    # window, and voids itself if it did not. That check is the point -- a
    # ProfileGPU that fires in the linger window is the same failure
    # tools/leg-summary.sh's header exists to prevent, and no wall-clock
    # heuristic catches it.
    [int]$CaptureAt       = 110,

    # Whole-sequence repeats. Arm order REVERSES on even repeats (B,A instead
    # of A,B) so that "arm A always went first" cannot be confounded with the
    # effect. Use this when the claimed effect is near the ~2% noise floor.
    [int]$Repeats         = 1,

    # ENGAGEMENT LEGS: one per ARMED arm, at -EngagementPose, with HoleStats 1.
    # These are what print the arm's counters; see the header on why they
    # cannot be the same legs as the timing legs. -NoEngagement skips them and
    # the manifest records that the sweep has no proof of traffic.
    [switch]$NoEngagement,
    [string]$EngagementPose = '0,0',

    # Print every argument list and write every leg wrapper, launch nothing.
    [switch]$DryRun,

    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
)

$ErrorActionPreference = 'Stop'
$RepoRoot  = (Resolve-Path "$PSScriptRoot\..").Path
$SavedDir  = Join-Path $RepoRoot 'Saved'
$LegScript = Join-Path $PSScriptRoot 'voxel-run-flight-leg.ps1'
$WrapDir   = Join-Path $SavedDir ".sweep-$Prefix"
$Manifest  = Join-Path $SavedDir "$Prefix-manifest.tsv"

if (-not (Test-Path $LegScript)) { throw "REFUSING TO START: $LegScript is missing." }

function Say([string]$Text, [string]$Colour = 'Gray') { Write-Host $Text -ForegroundColor $Colour }

# ===========================================================================
# VALIDATION, BEFORE ANYTHING TAKES THE BOX
# ===========================================================================

# THE PIPE RULE. UE splits -ExecCmds on COMMA and nothing else. A '|' does not
# separate anything; it becomes part of the previous command's argument and
# everything after it is silently dropped. That voided a five-arm experiment
# and every arm reported success.
foreach ($pair in @(@('BaseCvars', $BaseCvars), @('ArmACvars', $ArmACvars), @('ArmBCvars', $ArmBCvars))) {
    if ($pair[1] -match '\|') {
        throw ("REFUSING TO START: -$($pair[0]) contains a '|'. UE splits -ExecCmds on COMMA only; " +
               "a pipe is swallowed into the preceding command's argument and every cvar after it " +
               "is silently dropped while the leg reports success. Join with ', '.")
    }
    # This script owns HoleStats -- see the header. An arm that sets it is
    # asking for a timing leg and an engagement leg to be the same leg.
    if ($pair[1] -match 'voxel\.March\.HoleStats') {
        throw ("REFUSING TO START: -$($pair[0]) sets voxel.March.HoleStats. That cvar is a SHADER " +
               "PERMUTATION of the timed kernel (measured +2.2% at the horizon) and it is also the " +
               "switch that makes the arm counters print. This script sets it to 0 on timing legs " +
               "and 1 on engagement legs so the two are never the same leg. Remove it from the arm " +
               "string; use -NoEngagement if you genuinely want no engagement leg.")
    }
}

# THE PROOF RULE. See -NoProofRequired.
$Arms = @()
$Arms += [pscustomobject]@{ Tag = 'A'; Name = $ArmAName; Cvars = $ArmACvars; Proof = $ArmAProof }
if ($ArmBName -ne '' -or $ArmBCvars -ne '') {
    $bName = $ArmBName
    if ($bName -eq '') { $bName = 'armB' }
    $Arms += [pscustomobject]@{ Tag = 'B'; Name = $bName; Cvars = $ArmBCvars; Proof = $ArmBProof }
}
foreach ($arm in $Arms) {
    if ($arm.Cvars -ne '' -and $arm.Proof -eq '' -and -not $NoProofRequired) {
        throw ("REFUSING TO START: arm $($arm.Tag) ('$($arm.Name)') sets cvars but named no " +
               "-Arm$($arm.Tag)Proof pattern. An arm that cannot prove it carried traffic cannot " +
               "be believed when it reports a saving -- this project shipped eleven such arms in " +
               "one night. Give a regex that matches ONLY when the arm did work (e.g. " +
               "'blkConsulted=[1-9]'), not one that matches an echoed cvar (e.g. 'armed=1'). " +
               "-NoProofRequired overrides and says so in every line of the output.")
    }
}
if (($Arms.Count -eq 2) -and ($Arms[0].Name -eq $Arms[1].Name)) {
    throw "REFUSING TO START: both arms are named '$($Arms[0].Name)'. Their logs would overwrite each other."
}

# Poses.
$PoseList = @()
foreach ($p in $Poses) {
    $bits = $p -split ','
    if ($bits.Count -ne 2) { throw "REFUSING TO START: pose '$p' is not 'pitch,yaw'." }
    $pitch = 0.0; $yaw = 0.0
    if (-not [double]::TryParse($bits[0].Trim(), [ref]$pitch)) { throw "REFUSING TO START: pose '$p' has a non-numeric pitch." }
    if (-not [double]::TryParse($bits[1].Trim(), [ref]$yaw))   { throw "REFUSING TO START: pose '$p' has a non-numeric yaw." }
    if ($pitch -lt -90 -or $pitch -gt 90) {
        throw ("REFUSING TO START: pitch $pitch is outside -90..90. FRotator normalises it and the " +
               "pose the run pins would not be the pose the log name claims.")
    }
    $PoseList += [pscustomobject]@{ Pitch = $pitch; Yaw = $yaw; Tag = ("p{0}y{1}" -f [int]$pitch, [int]$yaw) }
}
$hasDown = @($PoseList | Where-Object { $_.Pitch -le -89.5 }).Count -gt 0
if (-not $hasDown) {
    Say "  NOTE: no pitch -90 pose in the set. Every ratio-to-down column will read 'n/a'. The" 'Yellow'
    Say "  thesis under test is that cost tracks empty space crossed, and -90 is the pose with" 'Yellow'
    Say "  none of it -- without it there is no denominator." 'Yellow'
}

# The capture must land inside the FLIGHT phase, not the preflight and not the
# linger. This is the arithmetic check; the per-leg check reads the log's own
# timestamps afterwards, which is what actually decides.
if ($CaptureAt -lt ($PreflightSec + 5)) {
    throw ("REFUSING TO START: -CaptureAt $CaptureAt lands at or before the end of the " +
           "${PreflightSec}s preflight. ProfileGPU would photograph a world that is still " +
           "streaming and report it as a successful capture.")
}
if ($CaptureAt -gt ($PreflightSec + $RunSec - 5)) {
    throw ("REFUSING TO START: -CaptureAt $CaptureAt lands in or after the linger window " +
           "(flight ends at $($PreflightSec + $RunSec)s). A capture taken there measures having " +
           "stopped -- the same trap tools/leg-summary.sh exists to prevent.")
}

# ===========================================================================
# THE BOX, AND THE BUILD. Checked ONCE up front so a 12-leg sweep does not
# discover on leg 1 that it was never going to run. voxel-run-flight-leg.ps1
# enforces both again per leg; this is the early, cheap refusal.
# ===========================================================================
$busy = @(Get-Process UnrealEditor-Cmd, UnrealEditor, cl, link, UnrealBuildTool, MSBuild, dotnet -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
    $detail = ($busy | ForEach-Object { "PID $($_.Id) ($($_.ProcessName))" }) -join ', '
    if ($DryRun) {
        Say "  DRY RUN: the box is BUSY -- $detail. A real sweep would refuse here." 'Yellow'
    } else {
        throw ("REFUSING TO START: $($busy.Count) process(es) hold the box -- $detail. Two legs " +
               "sharing the GPU produce contended numbers that look exactly like a slow " +
               "configuration, and a build that starts while an editor holds the shader DLL leaves " +
               "one DLL of the pair relinked and the other stale.")
    }
}

$ShadersDll = Join-Path $RepoRoot 'ue-project\Binaries\Win64\UnrealEditor-VoxelEarthShaders.dll'
$GameDll    = Join-Path $RepoRoot 'ue-project\Binaries\Win64\UnrealEditor-VoxelEarth.dll'
$CoreLib    = @((Join-Path $RepoRoot 'build\voxel-core-msvc\Release\voxelcore.lib'),
                (Join-Path $RepoRoot 'build\voxel-core-msvc\voxelcore.lib')) |
              Where-Object { Test-Path $_ } | Select-Object -First 1
$BuildStamp = Join-Path $RepoRoot 'Saved\.shader-build-stamp'
$shaderDirs = @((Join-Path $RepoRoot 'ue-project\Shaders'), (Join-Path $RepoRoot 'voxel-core\shaders')) |
              Where-Object { Test-Path $_ }
$NewestShader = $null
if ($shaderDirs.Count) {
    $NewestShader = Get-ChildItem -Path $shaderDirs -Recurse -Include *.usf,*.ush -ErrorAction SilentlyContinue |
                    Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
if ($NewestShader -and (Test-Path $ShadersDll)) {
    $ref = (Get-Item $ShadersDll).LastWriteTime
    if ((Test-Path $BuildStamp) -and (Get-Item $BuildStamp).LastWriteTime -gt $ref) { $ref = (Get-Item $BuildStamp).LastWriteTime }
    if ($NewestShader.LastWriteTime -gt $ref) {
        $skew = [int]($NewestShader.LastWriteTime - $ref).TotalSeconds
        $msg = ("SHADER TREE IS NEWER THAN THE BINARY: $($NewestShader.Name) is ${skew}s newer than " +
                "the last good build. Global shaders compile at BOOT from disk while their parameter " +
                "structures come from the DLL, so this sweep would measure a renderer that was never " +
                "built -- and the dangerous case is the one that still BINDS, which produces a " +
                "complete, plausible, silently-wrong table. Build first.")
        if ($DryRun) { Say "  DRY RUN: would refuse -- $msg" 'Yellow' } else { throw "REFUSING TO START: $msg" }
    }
}

# ===========================================================================
# THE MANIFEST. Written before the first leg, appended as legs complete.
# Tab-separated so tools/march-direction-summary.sh can read it with awk.
# ===========================================================================
function Stamp([string]$Path) {
    if ($Path -and (Test-Path $Path)) { return (Get-Item $Path).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') }
    return 'MISSING'
}
if (-not (Test-Path $SavedDir)) { New-Item -ItemType Directory -Path $SavedDir | Out-Null }
if (-not (Test-Path $WrapDir))  { New-Item -ItemType Directory -Path $WrapDir  | Out-Null }

$TAB = "`t"
$manifestLines = @()
$manifestLines += "# voxel-march-direction-sweep manifest -- read by tools/march-direction-summary.sh"
$manifestLines += "sweep$TAB$Prefix"
$manifestLines += "when$TAB$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))"
$manifestLines += "dryrun$TAB$([int]$DryRun.IsPresent)"
$manifestLines += "spawn$TAB$SpawnAt"
$manifestLines += "res$TAB${Width}x${Height}"
$manifestLines += "timing${TAB}preflight=$PreflightSec run=$RunSec linger=$LingerSec captureAt=$CaptureAt repeats=$Repeats"
$manifestLines += "base$TAB$BaseCvars"
$manifestLines += "proofrequired$TAB$([int](-not $NoProofRequired.IsPresent))"
$manifestLines += "bin${TAB}UnrealEditor-VoxelEarth.dll$TAB$(Stamp $GameDll)"
$manifestLines += "bin${TAB}UnrealEditor-VoxelEarthShaders.dll$TAB$(Stamp $ShadersDll)"
$manifestLines += "bin${TAB}voxelcore.lib$TAB$(Stamp $CoreLib)"
$manifestLines += "bin$TAB.shader-build-stamp$TAB$(Stamp $BuildStamp)"
if ($NewestShader) {
    $manifestLines += "bin${TAB}newest shader ($($NewestShader.Name))$TAB$($NewestShader.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"
}
foreach ($arm in $Arms) {
    $pf = $arm.Proof
    if ($pf -eq '') {
        if ($arm.Cvars -eq '') { $pf = '(control -- nothing to prove)' } else { $pf = 'NONE -- UNPROVEN' }
    }
    $manifestLines += "arm$TAB$($arm.Tag)$TAB$($arm.Name)$TAB$($arm.Cvars)$TAB$pf"
}
$manifestLines | Set-Content -Path $Manifest -Encoding UTF8

# ===========================================================================
# BUILDING ONE LEG
# ===========================================================================
#
# THE COMMA RULE. -ExecCmds is split by UE on COMMA. Every cvar this sweep adds
# goes through Join-Cvars, which drops empties and joins with ', ' -- never a
# pipe, never a bare space.
function Join-Cvars([string[]]$Parts) {
    $kept = @()
    foreach ($p in $Parts) {
        if ($null -ne $p -and $p.Trim() -ne '') { $kept += $p.Trim() }
    }
    return ($kept -join ', ')
}

# The leg is run through a GENERATED WRAPPER rather than by handing arguments to
# powershell -File. Two reasons, both measured on this box:
#   * `powershell -File leg.ps1 -ExtraArgs "-a","-b"` COLLAPSES the array into
#     one element "-a,-b" -- precisely the comma-glued token the ad-hoc legs
#     were passing, which only ever worked by FParse substring luck -- and
#     writing the two as separate quoted tokens SILENTLY DROPS the second.
#   * `powershell -Command "& leg.ps1 ..."` keeps the array but squashes the
#     child's exit code to 1, so a void leg and a good leg look the same.
# A wrapper with a splatted hashtable and `exit $LASTEXITCODE` gets both right,
# and it leaves on disk an exact, re-runnable record of what each leg was.
function New-LegWrapper($LogName, $Cvars, $Pitch, $Yaw) {
    $wrap = Join-Path $WrapDir "$LogName.ps1"
    $q = $Cvars -replace "'", "''"
    $lines = @()
    $lines += "# Generated by tools/voxel-march-direction-sweep.ps1 -- one leg of sweep '$Prefix'."
    $lines += "# Re-run this file to reproduce this exact leg."
    $lines += '$ErrorActionPreference = ''Continue'''
    $lines += '$legArgs = @{'
    $lines += "    LogName        = '$LogName'"
    $lines += "    Cvars          = '$q'"
    $lines += "    Flight         = 'static'"
    $lines += "    SpawnAt        = '$SpawnAt'"
    $lines += "    Width          = $Width"
    $lines += "    Height         = $Height"
    $lines += "    PreflightSec   = $PreflightSec"
    $lines += "    RunSec         = $RunSec"
    $lines += "    LingerSec      = $LingerSec"
    $lines += "    LogIntervalSec = $LogIntervalSec"
    $lines += "    TimeoutSec     = $TimeoutSec"
    $lines += "    Editor         = '$Editor'"
    $lines += "    TimeOfDay      = '12:00'"
    $lines += "    Date           = '03-20'"
    $lines += "    TimeScale      = 0"
    $lines += "    ExtraArgs      = @('-VoxelPerfPitch=$Pitch', '-VoxelPerfYaw=$Yaw')"
    $lines += '}'
    $lines += '$global:LASTEXITCODE = 0'
    $lines += "& '$LegScript' @legArgs"
    $lines += 'exit $LASTEXITCODE'
    $lines | Set-Content -Path $wrap -Encoding UTF8
    return $wrap
}

# ===========================================================================
# READING ONE LEG BACK. Every check below names its own failure.
# ===========================================================================
function Get-LogStamp([string]$Line) {
    # [2026.08.25-21.25.15:859][543]  ->  DateTime
    if ($Line -match '\[(\d{4})\.(\d{2})\.(\d{2})-(\d{2})\.(\d{2})\.(\d{2}):(\d{3})\]') {
        return (New-Object DateTime ([int]$Matches[1]), ([int]$Matches[2]), ([int]$Matches[3]),
                                    ([int]$Matches[4]), ([int]$Matches[5]), ([int]$Matches[6]), ([int]$Matches[7]))
    }
    return $null
}

# The ms of a ProfileGPU row. The row carries TWO time columns (self and
# inclusive) and then the pass name; a pass name never contains " ms ", so the
# LAST match on the line is the inclusive figure, which is the one quoted.
function Get-PassMs($Lines, [string]$Needle) {
    $row = $Lines | Where-Object { $_ -like '*LogRHI*' -and $_ -like "*$Needle*" } | Select-Object -Last 1
    if (-not $row) { return $null }
    $m = [regex]::Matches($row, '([0-9]+\.[0-9]+) ms')
    if ($m.Count -eq 0) { return $null }
    return [double]$m[$m.Count - 1].Groups[1].Value
}

function Test-Leg($LogName, $Pitch, $Yaw, $ProofRegex, $ExpectExecCmds, $ElapsedSec, $ExpectedSec, $Kind) {
    $r = [pscustomobject]@{
        Log = $LogName; Kind = $Kind; Pitch = $Pitch; Yaw = $Yaw
        View = 'n/a'; MarchMs = $null; FrameMs = $null; ScopeMs = $null
        P50 = $null; P95 = $null; Frames = $null; EmitFrames = $null
        TilesTotal = $null; TilesDrawn = $null
        Voids = @(); Warns = @()
    }
    $path = Join-Path $SavedDir "$LogName.log"
    if (-not (Test-Path $path)) {
        $r.Voids += 'NO LOG -- the leg never started (read the wrapper output above for the refusal)'
        return $r
    }
    $lines = Get-Content -Path $path -ErrorAction Stop

    # 1. WALL CLOCK. A short log is not a fast run.
    if ($ElapsedSec -ge 0 -and $ElapsedSec -lt ($ExpectedSec * 0.9)) {
        $r.Voids += "WALL CLOCK ${ElapsedSec}s against ~${ExpectedSec}s expected (under 0.9x)"
    }

    # 2. THE RUN'S OWN COMPLETION WITNESS.
    $complete = $lines | Where-Object { $_ -match 'VoxelPerfRun complete' } | Select-Object -Last 1
    if (-not $complete) {
        $r.Voids += 'NO "VoxelPerfRun complete" -- FinishRun never ran, so the phase this leg claims to measure did not finish'
    } else {
        if ($complete -match 'p50=([0-9.]+)ms') { $r.P50 = [double]$Matches[1] }
        if ($complete -match 'p95=([0-9.]+)ms') { $r.P95 = [double]$Matches[1] }
    }

    # 3. THE CVARS ACTUALLY REACHED THE PROCESS. Compares the editor's own
    #    echoed command line against what was asked for. This is what catches
    #    the comma/pipe/quoting family at the only place it matters.
    $cmdline = $lines | Where-Object { $_ -match 'LogInit: Command Line:' } | Select-Object -First 1
    if (-not $cmdline) {
        $r.Voids += 'NO "LogInit: Command Line:" -- cannot verify this leg was configured as asked'
    } elseif ($cmdline -notlike ('*-ExecCmds="' + $ExpectExecCmds + '"*')) {
        $r.Voids += 'THE -ExecCmds THE EDITOR SAW IS NOT THE ONE REQUESTED (quoting or comma loss)'
    }

    # 4. THE POSE. A direction sweep whose legs looked somewhere else is worse
    #    than no sweep. -VoxelPerfFlight static logs the pose it pinned.
    $pose = $lines | Where-Object { $_ -match 'STATIC pose pinned at' } | Select-Object -First 1
    if (-not $pose) {
        $r.Voids += 'NO "STATIC pose pinned" -- the run was not static and the direction is unknown'
    } elseif ($pose -match 'yaw=(-?[0-9.]+) pitch=(-?[0-9.]+)') {
        $gotYaw = [double]$Matches[1]; $gotPitch = [double]$Matches[2]
        if ([math]::Abs($gotPitch - $Pitch) -gt 0.05 -or [math]::Abs($gotYaw - $Yaw) -gt 0.05) {
            $r.Voids += "POSE MISMATCH: asked pitch=$Pitch yaw=$Yaw, the run pinned pitch=$gotPitch yaw=$gotYaw"
        }
    }

    # 5. view=. READ THE ENGINE, NEVER THE REQUEST.
    #    view=1552x873 at -Width 2560 is CORRECT -- a 60.6% screen percentage
    #    that TSR upscales to the owner's 1440p. It is NOT an invalidator and
    #    this script does not "fix" it. What IS an invalidator is the view
    #    CHANGING BETWEEN LEGS of one sweep (checked by the caller), because
    #    then the per-direction ms are ray counts of different sizes.
    $vm = $lines | Where-Object { $_ -match 'view=(\d+)x(\d+) px' } | Select-Object -First 1
    if ($vm -and $vm -match 'view=(\d+)x(\d+) px') {
        $r.View = "$($Matches[1])x$($Matches[2])"
    } else {
        $r.Voids += 'NO "view=" LINE -- the render size is unknown, so no number from this leg may be quoted'
    }

    # 6. THE CAPTURE FIRED, AND FIRED IN THE FLIGHT.
    $defer = $lines | Where-Object { $_ -match 'DeferExec: running now: ProfileGPU' } | Select-Object -First 1
    if ($Kind -eq 'timing') {
        if (-not $defer) {
            $r.Voids += 'ProfileGPU NEVER FIRED (no "DeferExec: running now: ProfileGPU")'
        } elseif ($pose -and $complete) {
            $tPose = Get-LogStamp $pose; $tCap = Get-LogStamp $defer; $tEnd = Get-LogStamp $complete
            if ($tPose -and $tCap -and $tEnd) {
                if ($tCap -lt $tPose) {
                    $r.Voids += 'THE CAPTURE FIRED BEFORE THE POSE WAS PINNED -- it photographed the preflight'
                } elseif ($tCap -gt $tEnd.AddSeconds(-1 * $LingerSec)) {
                    $r.Voids += ('THE CAPTURE FIRED IN THE LINGER WINDOW, which measures having stopped, not ' +
                                 'the flight -- lower -CaptureAt or raise -RunSec')
                } else {
                    $into = [int]($tCap - $tPose).TotalSeconds
                    $r.Warns += "capture landed ${into}s into the ${RunSec}s flight"
                }
            }
        }
    }

    # 7. THE NUMBER. VoxelMarch.March from ProfileGPU and nothing else.
    $r.MarchMs = Get-PassMs $lines 'VoxelMarch.March('
    $r.FrameMs = Get-PassMs $lines 'Frame '
    if ($Kind -eq 'timing' -and $null -eq $r.MarchMs) {
        $r.Voids += 'NO "VoxelMarch.March(" ROW IN THE PROFILE -- there is no GPU number in this leg'
    }

    # 8. DOUBLE GRANT. The allocator's own correctness gate. Non-zero means the
    #    GPU handed out dwords somebody already held and the colliding claims
    #    were FAILED -- the frame is missing geometry it should have marched,
    #    which reads as a saving.
    $dg = @($lines | Where-Object { $_ -match '\[brick-gpualloc\] DOUBLE GRANT' })
    if ($dg.Count -gt 0) { $r.Voids += "DOUBLE GRANT x$($dg.Count) -- the allocator's own correctness gate fired" }

    # 9. FINE TIER GATE LEAK. An elevation query answered from a non-resident
    #    tile reads as SEA LEVEL, which makes real ground "provably air" -- and
    #    an empty-space mechanism measured against ground that is not there
    #    reports a saving it did not make.
    $gl = @($lines | Where-Object { $_ -match 'FINE TIER GATE LEAK' })
    if ($gl.Count -gt 0) { $r.Voids += "FINE TIER GATE LEAK x$($gl.Count) -- the world under this leg is not the baked world" }

    # 10. THE MARCH RAN AND ITS OUTPUT WAS CONSUMED. emitFrames behind frames
    #     means the emit declined and RDG CULLED THE MARCH -- the timing
    #     brackets are NeverCull so they still report, describing work that was
    #     thrown away. This is the one way the instrument prints a plausible
    #     small number and is believed.
    $stats = $lines | Where-Object { $_ -match 'Voxel march: mode=' } | Select-Object -Last 1
    if ($stats) {
        if ($stats -match 'frames=(\d+) emitFrames=(\d+)') {
            $r.Frames = [int64]$Matches[1]; $r.EmitFrames = [int64]$Matches[2]
            if ($r.Frames -eq 0) {
                $r.Voids += 'MARCH frames=0 -- the pass never ran'
            } elseif ($r.EmitFrames -lt $r.Frames) {
                $r.Voids += ("emitFrames=$($r.EmitFrames) BEHIND frames=$($r.Frames) -- the emit declined and " +
                             'RDG culled the march; the ms describes discarded work')
            }
        }
        if ($stats -match 'tiles total=(\d+) drawn=(\d+)') {
            $r.TilesTotal = [int]$Matches[1]; $r.TilesDrawn = [int]$Matches[2]
        }
    } else {
        $r.Warns += 'no voxel.March.Stats line -- no independent witness that the march carried traffic'
    }

    # 11. PROOF OF TRAFFIC. The arm's own counter, matched by the arm's own
    #     pattern. "armed=1" is a cvar reading itself back and is not proof.
    if ($ProofRegex -ne '') {
        $hit = $lines | Where-Object { $_ -match $ProofRegex } | Select-Object -First 1
        if (-not $hit) { $r.Voids += "PROOF OF TRAFFIC NOT FOUND -- /$ProofRegex/ matched nothing in this log" }
    }

    return $r
}

# ===========================================================================
# THE SEQUENCE
# ===========================================================================
$expectedSec = $PreflightSec + $RunSec + $LingerSec
$plan = @()

# Engagement legs first: they are cheap proof, and if an arm turns out to be
# armed and inert the timing legs behind it are not worth the box time.
if (-not $NoEngagement) {
    $ebits = $EngagementPose -split ','
    if ($ebits.Count -ne 2) { throw "REFUSING TO START: -EngagementPose '$EngagementPose' is not 'pitch,yaw'." }
    $ePitch = [double]$ebits[0].Trim(); $eYaw = [double]$ebits[1].Trim()
    foreach ($arm in $Arms) {
        if ($arm.Cvars -eq '') { continue }   # the control has nothing to prove
        $plan += [pscustomobject]@{
            Kind = 'engagement'; Arm = $arm; Pitch = $ePitch; Yaw = $eYaw; Repeat = 0
            LogName = "$Prefix-$($arm.Name)-eng"
        }
    }
}

for ($rep = 1; $rep -le $Repeats; $rep++) {
    # ALTERNATED, NEVER GROUPED. Poses are the outer loop and arms the inner
    # one, so the sequence is A,B,A,B,... and the two legs of each comparison
    # are ADJACENT IN TIME -- the one pairing drift cannot fake. On even repeats
    # the arm order reverses, so "A always went first" is not confounded with
    # the effect either.
    foreach ($pose in $PoseList) {
        $order = $Arms
        if ((($rep % 2) -eq 0) -and $Arms.Count -gt 1) { $order = @($Arms[1], $Arms[0]) }
        foreach ($arm in $order) {
            $suffix = ''
            if ($Repeats -gt 1) { $suffix = "-r$rep" }
            $plan += [pscustomobject]@{
                Kind = 'timing'; Arm = $arm; Pitch = $pose.Pitch; Yaw = $pose.Yaw; Repeat = $rep
                LogName = "$Prefix-$($arm.Name)-$($pose.Tag)$suffix"
            }
        }
    }
}

$mins = [math]::Round((($plan.Count * ($expectedSec + 25)) / 60.0), 1)
$armDesc = @()
foreach ($a in $Arms) {
    $c = $a.Cvars
    if ($c -eq '') { $c = 'control, base cvars only' }
    $armDesc += "$($a.Tag)=$($a.Name) [$c]"
}
Say ''
Say "voxel-march-direction-sweep  '$Prefix'" 'Cyan'
Say "  arms      : $($armDesc -join '  |  ')"
Say "  poses     : $((($PoseList | ForEach-Object { "pitch $($_.Pitch) yaw $($_.Yaw)" }) -join ' ; '))"
Say "  spawn     : $SpawnAt   requested ${Width}x${Height}   sun frozen 12:00 03-20"
Say "  legs      : $($plan.Count)  (~$mins min)   manifest: Saved\$Prefix-manifest.tsv"
Say "  the number: VoxelMarch.March from ProfileGPU. Not marchMs, not frame time."
if ($NoProofRequired) { Say "  PROOF OF TRAFFIC NOT REQUIRED -- this sweep proves NOTHING about engagement." 'Red' }
if ($NoEngagement)    { Say "  NO ENGAGEMENT LEGS -- no arm in this sweep proves it carried traffic." 'Red' }
Say ''

$results = @()
$refView = $null
$i = 0
foreach ($leg in $plan) {
    $i++
    # HoleStats is set by the SWEEP, never by an arm. See the header.
    $hole = '0'
    if ($leg.Kind -eq 'engagement') { $hole = '1' }
    $execParts = @(
        $BaseCvars,
        "voxel.March.HoleStats $hole",
        $leg.Arm.Cvars,
        'r.ProfileGPU.ShowUI 0'
    )
    if ($leg.Kind -eq 'timing') {
        # ProfileGPU first, then the stats dump three seconds later so the two
        # never share a frame. Both go through voxel.DeferExec because
        # -ExecCmds fires at startup and would profile frame 1 of an empty
        # world -- and report it as a successful capture.
        $execParts += "voxel.DeferExec $CaptureAt ProfileGPU"
        $execParts += "voxel.DeferExec $($CaptureAt + 3) voxel.March.Stats"
    } else {
        $execParts += "voxel.DeferExec $CaptureAt voxel.March.Stats"
    }
    $exec = Join-Cvars $execParts
    $wrap = New-LegWrapper $leg.LogName $exec $leg.Pitch $leg.Yaw

    $label = "[{0,2}/{1}] {2,-28} {3,-16} pitch={4,4} yaw={5,4}" -f `
             $i, $plan.Count, $leg.LogName, "$($leg.Arm.Name)/$($leg.Kind)", $leg.Pitch, $leg.Yaw

    if ($DryRun) {
        Say $label 'Cyan'
        Say "         powershell -NoProfile -ExecutionPolicy Bypass -File `"$wrap`""
        Say "         which runs voxel-run-flight-leg.ps1 with:"
        Say "             -LogName        $($leg.LogName)"
        Say "             -Flight         static"
        Say "             -SpawnAt        $SpawnAt"
        Say "             -Width $Width -Height $Height"
        Say "             -PreflightSec $PreflightSec -RunSec $RunSec -LingerSec $LingerSec -LogIntervalSec $LogIntervalSec -TimeoutSec $TimeoutSec"
        Say "             -TimeOfDay 12:00 -Date 03-20 -TimeScale 0"
        Say "             -ExtraArgs      @('-VoxelPerfPitch=$($leg.Pitch)', '-VoxelPerfYaw=$($leg.Yaw)')"
        Say "             -Cvars          `"$exec`""
        if ($leg.Arm.Proof -ne '' -and $leg.Kind -eq 'engagement') {
            Say "         proof of traffic required: /$($leg.Arm.Proof)/"
        }
        Say ''
        continue
    }

    Say $label 'Cyan'
    $started = Get-Date
    $child = Start-Process -FilePath 'powershell.exe' -NoNewWindow -Wait -PassThru `
             -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$wrap`"")
    $elapsed = [int]((Get-Date) - $started).TotalSeconds

    $proof = ''
    if ($leg.Kind -eq 'engagement') { $proof = $leg.Arm.Proof }
    $res = Test-Leg $leg.LogName $leg.Pitch $leg.Yaw $proof $exec $elapsed $expectedSec $leg.Kind
    $res | Add-Member -NotePropertyName Arm       -NotePropertyValue $leg.Arm.Name
    $res | Add-Member -NotePropertyName ArmTag    -NotePropertyValue $leg.Arm.Tag
    $res | Add-Member -NotePropertyName Repeat    -NotePropertyValue $leg.Repeat
    $res | Add-Member -NotePropertyName Exec      -NotePropertyValue $exec
    $res | Add-Member -NotePropertyName ChildExit -NotePropertyValue $child.ExitCode

    # THE VIEW MUST NOT MOVE BETWEEN LEGS. See check 5.
    if ($res.View -ne 'n/a') {
        if (-not $refView) { $refView = $res.View }
        elseif ($res.View -ne $refView) {
            $res.Voids += "view=$($res.View) DISAGREES WITH THIS SWEEP'S OTHER LEGS ($refView) -- different ray counts, not comparable"
        }
    }

    if ($res.Voids.Count -gt 0) {
        Say "         VOID -- $($res.Voids.Count) invalidator(s):" 'Red'
        foreach ($v in $res.Voids) { Say "           * $v" 'Red' }
    } else {
        $msTxt = 'n/a'
        if ($null -ne $res.MarchMs) { $msTxt = ('{0:N3} ms' -f $res.MarchMs) }
        Say "         ok (${elapsed}s) view=$($res.View)  VoxelMarch.March=$msTxt" 'Green'
    }
    foreach ($w in $res.Warns) { Say "           - $w" 'DarkGray' }
    $results += $res

    $st = 'ok'
    if ($res.Voids.Count -gt 0) { $st = 'VOID' }
    Add-Content -Path $Manifest -Encoding UTF8 -Value `
        ("leg$TAB$($leg.LogName)$TAB$($leg.Arm.Tag)$TAB$($leg.Arm.Name)$TAB$($leg.Pitch)$TAB$($leg.Yaw)$TAB$($leg.Repeat)$TAB$($leg.Kind)$TAB$st")
}

if ($DryRun) {
    Say "DRY RUN COMPLETE. $($plan.Count) leg wrapper(s) written under" 'Yellow'
    Say "  $WrapDir" 'Yellow'
    Say "Nothing was launched and the box was not touched." 'Yellow'
    Add-Content -Path $Manifest -Encoding UTF8 -Value "note${TAB}DRY RUN -- no leg was executed"
    exit 0
}

# ===========================================================================
# THE TABLE. Ratios are per arm, against that arm's own pitch -90 leg.
# ===========================================================================
Say ''
Say ('{0,-28} {1,-8} {2,6} {3,5} {4,-10} {5,9} {6,7} {7,7} {8}' -f `
     'leg', 'arm', 'pitch', 'yaw', 'view', 'march ms', '/down', 'p50 ms', 'status') 'White'
Say ('-' * 124)

$timing = @($results | Where-Object { $_.Kind -eq 'timing' })
foreach ($arm in $Arms) {
    $downSet = @($timing | Where-Object {
        $_.ArmTag -eq $arm.Tag -and $_.Pitch -le -89.5 -and $_.Voids.Count -eq 0 -and $null -ne $_.MarchMs })
    $down = $null
    if ($downSet.Count -gt 0) { $down = $downSet[0] }
    foreach ($r in @($results | Where-Object { $_.ArmTag -eq $arm.Tag })) {
        $ms = 'n/a'; $ratio = 'n/a'; $p50 = 'n/a'
        if ($null -ne $r.MarchMs) { $ms = '{0:N3}' -f $r.MarchMs }
        if ($down -and $null -ne $r.MarchMs -and $down.MarchMs -gt 0) { $ratio = '{0:N2}x' -f ($r.MarchMs / $down.MarchMs) }
        if ($null -ne $r.P50) { $p50 = '{0:N2}' -f $r.P50 }
        $status = 'ok'
        if ($r.Voids.Count -gt 0) { $status = "VOID: $($r.Voids[0])" }
        if ($r.Kind -eq 'engagement') {
            $ms = "($ms)"; $ratio = 'eng'
            if ($status -eq 'ok') { $status = 'ok -- HoleStats 1, ms NOT comparable' }
        }
        $colour = 'Gray'
        if ($r.Voids.Count -gt 0) { $colour = 'Red' }
        Say ('{0,-28} {1,-8} {2,6} {3,5} {4,-10} {5,9} {6,7} {7,7} {8}' -f `
             $r.Log, $r.Arm, $r.Pitch, $r.Yaw, $r.View, $ms, $ratio, $p50, $status) $colour
    }
}

$voidCount = @($results | Where-Object { $_.Voids.Count -gt 0 }).Count
Say ''
if ($voidCount -gt 0) {
    Say "$voidCount of $($results.Count) LEGS ARE VOID. Do not quote a comparison that crosses one." 'Red'
} else {
    Say "$($results.Count) legs, none void. view=$refView on every leg." 'Green'
}
Say "Manifest: $Manifest"
Say "Summary : bash tools/march-direction-summary.sh $Prefix"
if ($voidCount -gt 0) { exit 1 }
exit 0
