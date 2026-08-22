# THE FINAL COMPARISON. One driver, one table, one report: the quad raster path
# against the marcher, on frame time, loading, VRAM and geometry submitted.
#
# ==============================================================================
# WHY THIS EXISTS, AND WHY IT IS SHAPED LIKE THIS
# ==============================================================================
#
# This project's failure mode is not bad measurement. It is INSTRUMENTS THAT
# REPORT SUCCESS WHILE MEASURING NOTHING. Seven were found on 2026-08-19 alone:
# a stale vxc_gpu binary "passing"; a cvar force that was silently refused and
# announced anyway; r.ShadowQuality read from a log line that a later write
# overrode; a unity build reporting Succeeded for a file it never compiled; an
# out-of-band check that reduces to `Coord.Z & 63 >= 64` and CANNOT FIRE; a
# TOTAL line that summed per-item means across denominators differing by 870x
# and reversed a verdict; and a hitch counter read as variance when it is a step
# function of the median.
#
# The final number of a project cannot be produced by an instrument with that
# failure mode available to it. So this driver is built around five refusals,
# each corresponding to one of those seven:
#
#   R1  A LEG THAT CANNOT PROVE ITS OWN CONFIGURATION IS VOID, NOT REPORTED.
#       Every arm declares the cvars that DEFINE it. This resolves the LAST
#       write of each one and WHAT SET IT, across all four shapes UE uses (see
#       Resolve-CvarHistory), including the refusal warning that means a write
#       you can see in the log never took effect.
#
#   R2  TWO LEGS WITH DIFFERENT CONFIG FINGERPRINTS DO NOT GO IN ONE TABLE.
#       Shadows started working on 2026-08-19. 18.99 ms and 34.72 ms are BOTH
#       real and describe DIFFERENT GAMES. Each leg's fingerprint is computed
#       from its OWN log -- not from the files on disk, which changed twice that
#       day -- and a table is refused, by name, if two legs differ in any key
#       the axis did not declare as its independent variable.
#
#   R3  A METRIC WHOSE MEANING CHANGES BETWEEN ARMS IS LABELLED OR OMITTED.
#       `loaded=` counts GEOMETRY PUBLICATION and collapses to 3,243 from 50,504
#       under voxel.Brick.SuppressQuadMesh while the pool fills normally to
#       87,753. `hitches` counts frames over a FIXED 33.3 ms, so at a 34.7 ms
#       median it reports 70% of frames -- the median restated, not variance.
#       Both are declared in $MetricValidity and gated per leg, and the reason
#       prints IN THE CELL, never in a footnote.
#
#   R4  TOTALS ARE TOTAL-OVER-COUNT. NEVER A SUM OF PER-ITEM MEANS.
#       voxel.Brick.Stats prints a TOTAL that sums three per-chunk means whose
#       counts differ by 870x. It read 1.127 ms/chunk where total/count is
#       0.389. This driver parses each term's (ms, count) SEPARATELY and never
#       reads a printed total. Same rule for the gather census: cumulative
#       quads over cumulative gathers, differenced across the settled window.
#
#   R5  NOTHING IS READ BEFORE IT SETTLES, AND SETTLE MUST BE PROVED.
#       Settle is the FIRST `Voxel streaming:` window with jobsInFlight=0,
#       pendingJobs=0 AND loaded= at the run's PEAK -- never the last census
#       line, and never first-idle alone. First-idle alone is WRONG and is
#       recorded as such: the GPU fork produces genuine mid-fill lulls, and legs
#       "settled" at loaded=40,615 against a true 43,328, making the fork look
#       12% faster while being scored at 94% of the work. A lull is not a finish
#       line. Two further guards: some EARLIER window must have had work in
#       flight (otherwise "0 jobs" may mean nothing had started), and windows
#       must CONTINUE after it (otherwise the settled state was never held).
#
# ==============================================================================
# THE DEFAULT IS A DRY RUN, AND THAT IS DELIBERATE
# ==============================================================================
#
# ONE UE EDITOR PER BOX AND THE OWNER DRIVES IT. Without -Execute this script
# launches nothing: it prints the exact leg list, the exact argument line each
# leg would receive, and the verification each leg would face. The plan is
# GENERATED FROM THE SAME TABLE THAT WOULD RUN IT, so the plan cannot drift from
# what runs.
#
#   (default)    DRY RUN. Prints the plan. Launches nothing.
#   -SelfTest    Parse-only, against 2026-08-19 legs whose answers are already
#                in docs/measurements/armA-drawpath-ceiling-2026-08-19.txt --
#                INCLUDING two negative cases the driver must REFUSE. Launches
#                nothing. This is how the instrument is proved before it is
#                trusted, and it needs no editor.
#   -VerifyOnly  Parse + report existing named logs. Launches nothing.
#   -Execute     Spend legs, serialized, through tools/voxel-run-gpu-arm.ps1.
#
# ==============================================================================
# WHAT THIS DRIVER DOES NOT DO
# ==============================================================================
#
# It does not build, it does not commit, and it does not decide whether the
# marcher wins. As of 2026-08-19 IT DOES NOT WIN, and cannot until hierarchical
# empty-space skipping lands. The report says so in its own UNMEASURED section
# rather than projecting past it. See docs/final-comparison-method.md.

param(
    [ValidateSet('all','frame','loading','vram','geometry')]
    [string[]]$Axis = @('all'),

    # TWO IS THE FLOOR, not a default to lower. A single leg per arm cannot
    # produce a spread, and without a spread there is no way to say whether a
    # delta cleared the rig's noise. Ground rule 1: never conclude from a single
    # run.
    [int]$Replicates = 2,

    [string]$Tag = (Get-Date -Format 'yyyy-MM-dd'),

    # THE RIG'S WITHIN-CONFIG SPREAD: 0.18 ms (~1%), measured over four
    # A0-equivalent legs. Not a guess, and not a tolerance to widen. Any
    # frame-time delta below it is NOT A RESULT.
    [double]$NoiseFloorMs = 0.18,

    # THE LOADING AXIS FLOOR IS COARSER AND IT IS STRUCTURAL. The streaming
    # census prints every -VoxelPerfLogInterval seconds, so both ends of a
    # cold-fill span are quantised to that. Two intervals is the smallest
    # difference that cannot be an artefact of where a boundary fell. 35.4 s vs
    # 35.6 s was correctly reported as "indistinguishable at 5-second
    # resolution", not as "0.2 s slower".
    [double]$LoadingFloorSec = 10.0,

    [switch]$Execute,
    [switch]$VerifyOnly,
    [switch]$SelfTest,

    # -VerifyOnly: 'logname=armId' pairs.
    [string[]]$Legs,

    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$SavedDir = Join-Path $RepoRoot 'Saved'
$Runner   = Join-Path $PSScriptRoot 'voxel-run-gpu-arm.ps1'
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'docs\measurements' }

# The pose every arm shares. -61440,-61440 is the site every Arm A number on
# this project was taken at, and 'static' pins position AND rotation and logs
# the pose -- which removes streaming variance instead of averaging over it.
# That mode exists because unpinned "settled" legs produced 43 fps and 103 fps
# on identical scenes.
$Pose = @{
    SpawnAt = '-61440,-61440'
    Width   = 2560
    Height  = 1440
    Flight  = 'static'
    RunSec  = 150
    PreflightSec = 90
    LingerSec = 10
    LogInterval = 5
}

# ==============================================================================
# SECTION 1 -- THE ARM CATALOGUE
# ==============================================================================
#
# One row per arm. THE POINT OF A TABLE RATHER THAN A SCRIPT IS THAT THE ARM AND
# ITS CONTROL ARE DECLARED TOGETHER AND CANNOT DRIFT APART. An arm with no
# control is a configuration error, not an arm.
#
#   Defines = cvars that must be PROVEN to hold their claimed value in the leg's
#             own log, or the leg is VOID (R1).
#   Varies  = fingerprint keys this axis is ALLOWED to differ on. Everything
#             else must match between arm and control or R2 refuses the table.
#
# PREREQUISITES THAT ARE NOT OPTIONAL, AND EACH COST SOMEBODY A LEG:
#
#   voxel.Fluid.Enable 1   defaults to 0. The march view extension declines
#                          every frame without a published fluid occupancy
#                          volume -- EVEN ON SOURCE 1, because the game-thread
#                          hookup rides the fluid subsystem's tick. A marcher
#                          arm without it reports declined frames and no cost.
#   voxel.GPU.BrickPack 1  defaults to 0. voxel.March.Source 1 does NOT imply a
#                          populated brick pool; nothing populates it but this
#                          and voxel.Brick.PackOnCpu.
#   voxel.Stream.GPUCullStatsPeriod 60   defaults to 0, i.e. the gather census
#                          is SILENT. The geometry axis has no numbers without
#                          it, and neither does the shadow multiplier S.
#   voxel.Brick.Stats      is a console COMMAND, not a periodic line. It must be
#                          deferred to AFTER settle or it prints the preflight
#                          pool. Deferred here through -Cvars, which lands in
#                          the exec list ahead of the runner's own DeferExec.

# THE DEFERRED READS, AND WHY THESE SECONDS AND NOT OTHERS.
#
# Seconds are from process start, matching -CaptureAt in voxel-run-gpu-arm.ps1.
# The leg is 90 preflight + 150 run + 10 linger = 250 s, so every read must land
# inside that window or it never fires at all -- and a command that never fired
# leaves no trace beyond a missing line, which reads exactly like a feature that
# printed nothing to say.
#
#   200  voxel.Brick.Stats  -- well past settle (~35-45 s) and 50 s before exit.
#   205  voxel.March.Stats  -- first read.
#   235  voxel.March.Stats  -- SECOND read, 30 s later.
#
# THE SECOND MARCH READ IS NOT REDUNDANT. The depth-gate instrument moved
# 0.0094% -> 0.0342% between two reads of the same settled scene while each read
# was a single arbitrary frame; the fix was to make each read an average over 64
# frames, and the evidence the fix worked was that two reads then agreed to
# 0.002 percentage points. Two reads separated in time are what turns "this is
# the number" into "this number is stable". The parser takes the last; the pair
# is there for a human to check convergence, and for the ProfileGPU capture at
# 165 to sit clear of both.
$Deferred = 'voxel.DeferExec 200 voxel.Brick.Stats, voxel.DeferExec 205 voxel.March.Stats, voxel.DeferExec 235 voxel.March.Stats'
$Common   = "voxel.Stream.GPUCullStatsPeriod 60, voxel.Fluid.Enable 1, $Deferred"

$ArmCatalogue = @(
    # --------------------------------------------------------------------------
    # AXIS 1 -- FRAME TIME. The quad raster path against the marcher.
    #
    # This is the p3-ident pair, replicated. voxel.March 0 is byte-identical to
    # the shipped frame and costs nothing; voxel.March 2 adds the marcher and
    # writes a full scratch copy. VerifyDepth is OFF in both, because its
    # readback would land inside the number -- the pair that closed the identity
    # check confirmed this with "depth gate: NOT RUN" in both logs.
    #
    # THE IDENTITY CHECK RIDES THIS PAIR FOR FREE and the driver evaluates it:
    #   mode2frame - mode0frame == marchMs + emitMs + scratchMs
    # It closed to 0.137 ms (2.5%) against a 0.18 ms floor. If it stops closing,
    # something is being paid that no bracket names, and the frame-time row is
    # not trustworthy however clean it looks.
    # --------------------------------------------------------------------------
    [pscustomobject]@{
        Axis='frame'; Id='frame-raster'; Role='control'; Pair='frame'
        Label='OLD: quad raster path (voxel.March 0)'
        Cvars="voxel.March 0, voxel.March.VerifyDepth 0, voxel.GPU.BrickPack 1, $Common"
        Defines=@{ 'voxel.March'='0'; 'voxel.March.VerifyDepth'='0'; 'voxel.Fluid.Enable'='1' }
        Varies=@('voxel.March','voxel.March.Source')
    }
    [pscustomobject]@{
        Axis='frame'; Id='frame-march'; Role='arm'; Pair='frame'
        Label='NEW: marcher drawing, brick-pool source (voxel.March 2 + Source 1)'
        Cvars="voxel.March 2, voxel.March.Source 1, voxel.March.VerifyDepth 0, voxel.GPU.BrickPack 1, $Common"
        Defines=@{ 'voxel.March'='2'; 'voxel.March.Source'='1'; 'voxel.March.VerifyDepth'='0'; 'voxel.Fluid.Enable'='1' }
        Varies=@('voxel.March','voxel.March.Source')
    }

    # --------------------------------------------------------------------------
    # AXIS 2 -- LOADING. Cold fill, marcher-fed.
    #
    # THE METRIC HAD TO CHANGE AND THE REASON IS ON RECORD. Under
    # voxel.Brick.SuppressQuadMesh 1 the worker packs bricks and publishes no
    # geometry, so `loaded=` -- which counts geometry publication -- collapsed to
    # 3,243 against 50,504 while the brick pool filled normally to 87,753. The
    # pipeline was healthy; the instrument was measuring a different quantity. A
    # cold-fill comparison built on `loaded=` CANNOT cross this pair, and this
    # driver refuses to try.
    #
    # WHAT CROSSES IT: the per-term worker cost (mesh/fill/pack, each with its
    # own count) and packs/s, both of which mean the same thing on both arms.
    # Phase 5 measured 0.969 -> 0.389 ms/chunk, 2.49x, on exactly this pair.
    # --------------------------------------------------------------------------
    [pscustomobject]@{
        Axis='loading'; Id='load-today'; Role='control'; Pair='loading'
        Label='OLD: mesh quads + pack bricks (SuppressQuadMesh 0)'
        Cvars="voxel.Brick.SuppressQuadMesh 0, voxel.Brick.PackOnCpu 1, voxel.Brick.PackReuseMesherVoxels 1, voxel.GPU.BrickPack 1, $Common"
        Defines=@{ 'voxel.Brick.SuppressQuadMesh'='0'; 'voxel.Brick.PackOnCpu'='1'; 'voxel.Brick.PackReuseMesherVoxels'='1' }
        Varies=@('voxel.Brick.SuppressQuadMesh')
    }
    [pscustomobject]@{
        Axis='loading'; Id='load-march'; Role='arm'; Pair='loading'
        Label='NEW: marcher-fed -- pack bricks only, no quad meshing (SuppressQuadMesh 1)'
        Cvars="voxel.Brick.SuppressQuadMesh 1, voxel.Brick.PackOnCpu 1, voxel.Brick.PackReuseMesherVoxels 1, voxel.GPU.BrickPack 1, $Common"
        Defines=@{ 'voxel.Brick.SuppressQuadMesh'='1'; 'voxel.Brick.PackOnCpu'='1'; 'voxel.Brick.PackReuseMesherVoxels'='1' }
        Varies=@('voxel.Brick.SuppressQuadMesh')
    }

    # --------------------------------------------------------------------------
    # AXIS 3 -- VRAM AT SETTLE.
    #
    # BOTH POOLS ARE RESIDENT IN THE SAME LEG TODAY, because both producers run.
    # That makes this the strongest comparison the project has -- same frame,
    # same ground, same instant -- and it needs no arm switch at all. The
    # replicate IS the control here, and what it controls for is the rig.
    #
    # THE TRAP THIS AXIS EXISTS TO AVOID IS ALREADY ON RECORD: an earlier draft
    # of the plan claimed a 3-14x VRAM refund by comparing the brick RESIDENT set
    # against the quad pool's COMMITTED 2,197 MB, which is ~3x the cascade's own
    # quad content. Most of the apparent refund was allocator slack. The measured
    # format saving is 1.74x. So this driver emits resident-vs-resident and
    # committed-vs-committed AS TWO SEPARATE ROWS and refuses to cross them.
    # --------------------------------------------------------------------------
    [pscustomobject]@{
        Axis='vram'; Id='vram-both'; Role='arm'; Pair='vram'
        Label='BOTH pools resident in one leg (quad content vs brick content, same ground)'
        Cvars="voxel.Brick.PackOnCpu 1, voxel.GPU.BrickPack 1, voxel.March 0, $Common"
        Defines=@{ 'voxel.Brick.PackOnCpu'='1' }
        Varies=@()
    }

    # --------------------------------------------------------------------------
    # AXIS 4 -- GEOMETRY SUBMITTED.
    #
    # THE MARCHER'S "ZERO" IS MEASURED HERE, NOT ASSERTED. The quad path is not
    # retired -- that is P4 -- so the shipped marcher configuration does not
    # exist yet. What DOES exist is voxel.Stream.GPUCullDebugDrawNothing 3:
    # submit nothing, walk and emit still run. That is the Phase 4 SHAPE on
    # today's binary, and the report labels it as emulated rather than shipped.
    #
    # AND THE ZERO IS A MEASURED ZERO, NOT AN ABSENCE. Both suppression paths in
    # VoxelGpuPoolComponent call RecordGather BEFORE `continue`, deliberately, so
    # a suppressed gather appears in the census as a gather that submitted zero.
    # cameraGathers=0 would mean the instrument stopped; cameraGathers holding
    # with camQuads=0 means submission genuinely went to zero. The verifier below
    # checks exactly that distinction and voids the arm if gathers are absent.
    # --------------------------------------------------------------------------
    [pscustomobject]@{
        Axis='geometry'; Id='geom-raster'; Role='control'; Pair='geometry'
        Label='OLD: quad submission live (shipped draw path)'
        Cvars="voxel.March 0, voxel.Stream.GPUCullDebugDrawNothing 0, voxel.GPU.BrickPack 1, $Common"
        Defines=@{ 'voxel.Stream.GPUCullDebugDrawNothing'='0'; 'voxel.March'='0' }
        Varies=@('voxel.March','voxel.March.Source','voxel.Stream.GPUCullDebugDrawNothing')
    }
    [pscustomobject]@{
        Axis='geometry'; Id='geom-march'; Role='arm'; Pair='geometry'
        Label='NEW: marcher drawing, quad submission suppressed (Phase 4 shape, EMULATED)'
        Cvars="voxel.March 2, voxel.March.Source 1, voxel.Stream.GPUCullDebugDrawNothing 3, voxel.GPU.BrickPack 1, $Common"
        Defines=@{ 'voxel.Stream.GPUCullDebugDrawNothing'='3'; 'voxel.March'='2'; 'voxel.March.Source'='1' }
        Varies=@('voxel.March','voxel.March.Source','voxel.Stream.GPUCullDebugDrawNothing')
    }
)

# ==============================================================================
# SECTION 2 -- THE FINGERPRINT KEY SET
# ==============================================================================
#
# EVERY KEY HERE EARNED ITS PLACE BY HAVING SILENTLY CHANGED A RESULT ON THIS
# PROJECT. This is not a precaution list; it is a scar list.
#
#   r.ShadowQuality              was 0 in every leg ever taken until
#                                2026-08-19, from a stale sg.ShadowQuality=0
#                                applied by the SECOND of two boot scalability
#                                passes. It shut CreateDynamicShadows entirely.
#                                +15.83 ms at p50 when opened.
#   r.Shadow.CSM.MaxCascades     the same @0 block capped cascades to 1 and
#   r.Shadow.DistanceScale       scaled distance to 0.6, so TWO settings that
#                                were confirmed applied had been made
#                                meaningless by a third.
#   r.Shadow.UseOctreeForCulling 0 is what makes the pool visible to the shadow
#                                gather at all -- and is itself a change whose
#                                own cost is unmeasured.
#   voxel.GI.Enabled/.Volume     both default to 1 despite help text saying
#                                "0 = off (default)". Every baseline contains GI.
#   voxel.Fluid.Enable           defaults to 0 and gates the marcher entirely.
#   voxel.March*                 the independent variable on two axes.
#   voxel.Brick.*                the independent variable on loading, plus the
#                                reuse control whose absence turns 0.163
#                                ms/chunk into 0.608.
#   voxel.Stream.GPU*            submission, cull suppression, and the census
#                                that is silent at its default.
#   voxel.GPU.*                  producer path and batch caps.

$FingerprintCvars = @(
    'r.ShadowQuality'
    'r.Shadow.CSM.MaxCascades'
    'r.Shadow.MaxCSMResolution'
    'r.Shadow.DistanceScale'
    'r.Shadow.RadiusThreshold'
    'r.Shadow.Virtual.Enable'
    'r.Shadow.UseOctreeForCulling'
    'r.Shadow.CSMCaching'
    'voxel.GI.Enabled'
    'voxel.GI.Volume'
    'voxel.Fluid.Enable'
    'voxel.March'
    'voxel.March.Source'
    'voxel.March.VerifyDepth'
    'voxel.March.StepBudget'
    'voxel.March.AO'
    'voxel.March.HTileProbe'
    'voxel.Stream.GPU'
    'voxel.Stream.GPUCull'
    'voxel.Stream.GPUCullDebugDrawNothing'
    'voxel.Stream.GPUShadowMaxLevel'
    'voxel.GPU.MeshDirectToPool'
    'voxel.GPU.BrickPack'
    'voxel.GPU.BrickPackResident'
    'voxel.GPU.MeshBatchCap'
    'voxel.Brick.PackOnCpu'
    'voxel.Brick.SuppressQuadMesh'
    'voxel.Brick.PackReuseMesherVoxels'
)

# Command-line dimensions that are NOT cvars and CANNOT be reached by -ExecCmds.
# Ring composition, pool capacity, flight mode and the GPU fork's in-flight cap
# are all resolved into function-local statics before the first
# RecomputeDesiredSet, i.e. before any console command has run. A sweep over any
# of them is one leg per value, and a leg that got them wrong cannot be fixed
# afterwards -- so they belong in the fingerprint, not in a comment.
$FingerprintCmdline = @(
    'ResX','ResY','VoxelSpawnAt','VoxelPerfFlight','VoxelPerfLogInterval',
    'VoxelTimeOfDay','VoxelDate','VoxelTimeScale',
    'VoxelMaxRingLevel','VoxelRingInnerMeters','VoxelRingOuterMeters',
    'VoxelPoolCapacityQuads','VoxelGpuMeshInFlight','VoxelGpuMeshMaxLevel'
)

# ==============================================================================
# SECTION 3 -- CVAR RESOLUTION: THE LAST WRITE, AND WHAT SET IT
# ==============================================================================
#
# UE WRITES CVARS INTO THE LOG IN FOUR DIFFERENT SHAPES, AND A SCAN THAT KNOWS
# ONLY ONE WILL CONFIDENTLY REPORT THE WRONG VALUE. Verified against
# Saved/p0-sh-quality-ctl.log, where the shapes disagree with each other:
#
#   line  671  LogConfig: Applying CVar settings from Section [ShadowQuality@3] File [Scalability]
#   line  673  LogConfig: Set CVar [[r.ShadowQuality:5]]
#   line  862  LogConfig: Applying CVar settings from Section [ShadowQuality@0] File [Scalability]
#   line  864  LogConfig: Set CVar [[r.ShadowQuality:0]]
#   line  938  LogConfig: Applying CVar settings from Section [ConsoleVariables] File [Engine]
#   line  941  LogConfig: Set CVar [[r.ShadowQuality:5]]
#   line 1807  r.ShadowQuality = "0"                        <- -ExecCmds, 18 s later
#
# THAT LAST LINE HAS NO LOG CATEGORY AT ALL. A scan for `Set CVar` -- which is
# the obvious scan, and the one a reasonable person writes -- stops at 941 and
# reports 5. The leg ran 0. That is the CONTROL leg of the shadow pair, so
# getting it wrong inverts the entire shadow result.
#
# SHAPE 4 IS THE ONE THAT MATTERS MOST AND IS EASIEST TO MISS: the engine's
# refusal notice.
#
#   LogConsoleManager: Warning: Setting the console variable 'X' with
#   'SetByCode' was ignored as it is lower priority than the previous
#   'SetByConsole'. Value remains '1'
#
# A write that was logged and then REFUSED did nothing. This is Defect 1 of
# 2026-08-19, where a force was refused and announced as applied anyway. The
# resolver records refusals and reports the value that actually remains.
#
# The setter is reported alongside the value, always, because priority decides
# who wins and a later write does not always take: SetByConsole (0x0D) outranks
# SetByConsoleVariablesIni (0x0A) outranks SetByCode (0x04) outranks
# SetByScalability (0x02).

function Resolve-CvarHistory {
    param([string[]]$Lines)

    $history = @{}
    $refusals = @{}
    $currentSection = 'unknown'

    $rxApply = [regex]'LogConfig:\s+Applying CVar settings from Section \[([^\]]+)\] File \[([^\]]+)\]'
    $rxSet   = [regex]'LogConfig:\s+Set CVar \[\[([A-Za-z0-9_.]+):(.*?)\]\]'
    # The runtime console echo. NO CATEGORY AT ALL. Anchored on the timestamp and
    # frame-counter prefix so ordinary prose containing ' = "' cannot match it.
    $rxEcho  = [regex]'^\[\d{4}\.\d{2}\.\d{2}-[\d.:]+\]\[[ \d]+\]([A-Za-z][A-Za-z0-9_]*\.[A-Za-z0-9_.]+) = "(.*?)"\s*$'
    $rxRefuse = [regex]"LogConsoleManager: Warning: Setting the console variable '([^']+)' with '(SetBy\w+)' was ignored as it is lower priority than the previous '(SetBy\w+)'\. Value remains '([^']*)'"

    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $line = $Lines[$i]

        $m = $rxApply.Match($line)
        if ($m.Success) {
            $currentSection = "$($m.Groups[2].Value):[$($m.Groups[1].Value)]"
            continue
        }
        $m = $rxSet.Match($line)
        if ($m.Success) {
            $name = $m.Groups[1].Value
            if (-not $history.ContainsKey($name)) { $history[$name] = @() }
            # Ini values can carry a trailing ini comment inside the brackets.
            $raw = ($m.Groups[2].Value -split ';')[0].Trim()
            $history[$name] += [pscustomobject]@{ Line=$i+1; Value=$raw; Setter=$currentSection }
            continue
        }
        $m = $rxEcho.Match($line)
        if ($m.Success) {
            $name = $m.Groups[1].Value
            if (-not $history.ContainsKey($name)) { $history[$name] = @() }
            $history[$name] += [pscustomobject]@{ Line=$i+1; Value=$m.Groups[2].Value.Trim(); Setter='console (ExecCmds/DeferExec)' }
            continue
        }
        $m = $rxRefuse.Match($line)
        if ($m.Success) {
            $name = $m.Groups[1].Value
            if (-not $refusals.ContainsKey($name)) { $refusals[$name] = @() }
            $refusals[$name] += [pscustomobject]@{
                Line=$i+1; Tried=$m.Groups[2].Value; Held=$m.Groups[3].Value; Remains=$m.Groups[4].Value
            }
            # The refusal is itself authoritative about the resulting value.
            if (-not $history.ContainsKey($name)) { $history[$name] = @() }
            $history[$name] += [pscustomobject]@{
                Line=$i+1; Value=$m.Groups[4].Value
                Setter="REFUSED $($m.Groups[2].Value); $($m.Groups[3].Value) still holds"
            }
        }
    }
    return [pscustomobject]@{ History=$history; Refusals=$refusals }
}

# Normalise the values the four shapes produce for one state. r.ProfileGPU.ShowUI
# echoes "false" where an ini writes "0"; a bool set from C++ may echo "True";
# 5.000000 and 5 are the same setting. Comparing raw strings would VOID legs for
# agreeing, which is a refusal that costs a night and teaches nothing.
function ConvertTo-CvarCanonical {
    param([string]$Value)
    if ($null -eq $Value) { return $null }
    $v = $Value.Trim().Trim('"')
    if ($v -match '^(?i:false|off|no)$') { return '0' }
    if ($v -match '^(?i:true|on|yes)$')  { return '1' }
    $parsed = 0.0
    if ([double]::TryParse($v, [System.Globalization.NumberStyles]::Float,
                           [cultureinfo]::InvariantCulture, [ref]$parsed)) {
        if ($parsed -eq [math]::Floor($parsed)) { return ([int]$parsed).ToString() }
        return $parsed.ToString('0.####', [cultureinfo]::InvariantCulture)
    }
    return $v
}

function Get-CvarFinal {
    param($Resolved, [string]$Name)
    $h = $Resolved.History
    if (-not $h.ContainsKey($Name)) {
        # NEVER WRITTEN is not the same as "at its default". A cvar this driver
        # never saw written may well be at its default -- but "the default says
        # so" is exactly the standard of evidence that failed on r.ShadowQuality,
        # so it is recorded as unproven and the arm decides whether that is fatal.
        return [pscustomobject]@{ Name=$Name; Value=$null; Raw=$null; Setter='NEVER WRITTEN IN THIS LOG'; Line=0; Known=$false; Refused=$false }
    }
    $last = $h[$Name][-1]
    $refused = $Resolved.Refusals.ContainsKey($Name)
    return [pscustomobject]@{
        Name=$Name; Value=(ConvertTo-CvarCanonical $last.Value); Raw=$last.Value
        Setter=$last.Setter; Line=$last.Line; Known=$true; Refused=$refused
    }
}

# ==============================================================================
# SECTION 4 -- SETTLE, PROVED RATHER THAN ASSUMED
# ==============================================================================
#
# Settle = the FIRST `Voxel streaming:` window with jobsInFlight=0 AND
# pendingJobs=0 AND loaded= equal to the run's PEAK loaded.
#
# THE PEAK CLAUSE IS THE ONE THAT COSTS A RESULT IF IT IS DROPPED. Idle alone is
# not convergence: on the CPU producer idle coincides with full residency, but
# the GPU fork produces genuine mid-fill lulls. Legs "settled" at loaded=40,615
# and 41,229 against a true final 43,328, making the fork look ~12% faster while
# being scored at 94% of the work. A lull is not a finish line.
#
# TWO FURTHER GUARDS, both from the same rule:
#   (a) some EARLIER window must have had work in flight. Otherwise "0 jobs" may
#       simply mean nothing had started, and two identical reads of a world that
#       never filled agree perfectly.
#   (b) windows must CONTINUE after the settle point. Otherwise the run ended at
#       settle and the settled state was never held long enough to read.
#
# Fill seconds are measured from the FIRST streaming window, which excludes
# process start, shader warm-up and map load.

function Get-SettlePoint {
    param([string[]]$Lines, [int]$MinWindowsAfter = 3)

    $rx = [regex]'Voxel streaming: loaded=(\d+).*?jobsInFlight=(\d+) pendingJobs=(\d+).*?residentQuads=(\d+)'
    $rxTime = [regex]'^\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2})'
    $windows = @()
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $m = $rx.Match($Lines[$i])
        if (-not $m.Success) { continue }
        $t = $rxTime.Match($Lines[$i])
        $stamp = $null
        if ($t.Success) { $stamp = [datetime]::ParseExact($t.Groups[1].Value, 'yyyy.MM.dd-HH.mm.ss', $null) }
        $windows += [pscustomobject]@{
            Index=$windows.Count; LineNo=$i+1; Loaded=[long]$m.Groups[1].Value
            InFlight=[int]$m.Groups[2].Value; Pending=[int]$m.Groups[3].Value
            ResidentQuads=[long]$m.Groups[4].Value; Time=$stamp
        }
    }
    if ($windows.Count -eq 0) {
        return [pscustomobject]@{ Settled=$false; Reason='no "Voxel streaming:" windows in this log'; Windows=@() }
    }

    $peak = ($windows | Measure-Object -Property Loaded -Maximum).Maximum
    $sawWork = $false
    for ($i = 0; $i -lt $windows.Count; $i++) {
        $w = $windows[$i]
        if ($w.InFlight -gt 0 -or $w.Pending -gt 0) { $sawWork = $true; continue }
        if (-not $sawWork) { continue }                 # guard (a)
        if ($w.Loaded -lt $peak) { continue }           # THE PEAK CLAUSE: a lull is not a finish line
        if (($windows.Count - 1 - $i) -lt $MinWindowsAfter) {
            return [pscustomobject]@{
                Settled=$false; Windows=$windows
                Reason=("settle candidate is window {0} of {1} -- only {2} windows follow it, under the {3} required. The run ended at settle; the settled state was never held." -f $i, $windows.Count, ($windows.Count-1-$i), $MinWindowsAfter)
            }
        }
        $span = $null
        if ($windows[0].Time -and $w.Time) { $span = ($w.Time - $windows[0].Time).TotalSeconds }
        return [pscustomobject]@{
            Settled=$true; Reason=''; Windows=$windows; SettleIndex=$i
            SettleLine=$w.LineNo; SettleWindow=$w; PeakLoaded=$peak
            FillSeconds=$span; TotalWindows=$windows.Count
        }
    }
    return [pscustomobject]@{
        Settled=$false; Windows=$windows; PeakLoaded=$peak
        Reason=("no window is simultaneously idle and at peak loaded={0} -- the world never converged in this leg" -f $peak)
    }
}

# ==============================================================================
# SECTION 5 -- METRIC VALIDITY: WHICH NUMBERS MEAN WHAT, ON WHICH ARM
# ==============================================================================
#
# R3. A metric that silently changes meaning between arms is worse than a
# missing one. Each entry returns $null when the metric is valid for that leg,
# or a STRING saying why it is not -- and that string prints IN THE CELL, beside
# the figure it disqualifies, never in a footnote.

$MetricValidity = @{

    # `loaded=` counts GEOMETRY PUBLICATION. Under quad-mesh suppression chunks
    # publish nothing, so it collapsed to 3,243 against 50,504 while the brick
    # pool filled normally to 87,753. The pipeline was healthy; the instrument
    # was measuring a different quantity.
    'loadedChunks' = {
        param($leg)
        $s = $leg.Cvars['voxel.Brick.SuppressQuadMesh']
        if ($s -and $s.Value -eq '1') {
            return 'INVALID ON THIS ARM: loaded= counts geometry publication, which SuppressQuadMesh 1 turns off. Read packs/s and the brick pool chunk count instead.'
        }
        return $null
    }
    'coldFillSec' = {
        param($leg)
        $s = $leg.Cvars['voxel.Brick.SuppressQuadMesh']
        if ($s -and $s.Value -eq '1') {
            return 'INVALID ON THIS ARM: this span is derived from the loaded=/jobsInFlight census, which SuppressQuadMesh 1 decouples from the pool fill.'
        }
        return $null
    }

    # THE HITCH COUNTER IS A STEP FUNCTION OF THE MEDIAN, NOT A MEASURE OF
    # VARIANCE. VoxelDebug.h:419, kHitchThresholdMs = 33.3f, fixed. At p50 18.85
    # it caught 1.4% of frames; at p50 34.72 it caught 70.3% -- and p50 34.72
    # against p95 35.38 is a 0.66 ms spread, which CANNOT have 70% outliers. It
    # was reporting "the frame is now slower than 30 fps", which the p50 already
    # said. This nearly became an investigation into a stutter that does not
    # exist.
    'hitches' = {
        param($leg)
        if ($null -eq $leg.FrameP50) { return 'no frame distribution in this leg' }
        if ($leg.FrameP50 -ge 26.6) {
            return ('DEGENERATE: fixed 33.3 ms threshold against a p50 of {0:N2} ms. This is the median restated, not variance.' -f $leg.FrameP50)
        }
        return $null
    }

    # p95 disagreed with ITSELF by 3.8 ms inside one leg -- 24.52 on the full-run
    # line against 28.31 on the post-warmup line -- and the pair it was quoted
    # across differed by 22% in frame count. A tail metric that cannot reproduce
    # within one leg cannot resolve a difference between two.
    'frameP95' = {
        param($leg)
        return 'TAIL UNRESOLVED ON THIS RIG: p95 has disagreed with itself by 3.8 ms inside a single leg. Printed for completeness; not a result.'
    }

    # Resident bytes bound the live set only while nothing has been evicted, and
    # the pool bounds itself by evicting in insertion order rather than failing.
    'brickResidentMiB' = {
        param($leg)
        if ($null -ne $leg.BrickEvictions -and $leg.BrickEvictions -gt 0) {
            return ('evictions={0}: resident bytes are no longer an upper bound on the live set.' -f $leg.BrickEvictions)
        }
        return $null
    }

    # The marcher's cost is only a cost result while it is actually hitting the
    # world. A miss runs to the full step budget and a hit terminates early, so
    # "hits far less, costs more" is the signature of rays failing to find
    # terrain that is there -- which is a symptom, not a price. The engine's own
    # guard fires below 15% of tiles; this mirrors it so the number never leaves
    # the harness unlabelled.
    'marchMs' = {
        param($leg)
        if ($null -eq $leg.MarchMs) { return 'no voxel.March.Stats in this leg' }
        if ($null -ne $leg.TilesTotal -and $leg.TilesTotal -gt 0 -and $null -ne $leg.TilesDrawn) {
            $pct = 100.0 * $leg.TilesDrawn / $leg.TilesTotal
            if ($pct -lt 15.0) {
                return ('NOT A COST RESULT: drew only {0:N0} of {1:N0} tiles ({2:N1}%). A miss runs to the full step budget, so this is mostly wasted stepping.' -f $leg.TilesDrawn, $leg.TilesTotal, $pct)
            }
        }
        if ($leg.MarchSource -eq 'occupancy') {
            return 'STAND-IN SOURCE: this is the 512-cubed fluid occupancy volume (51.2 m, one level, one bit per voxel, no material), not the brick pool the plan is about.'
        }
        return $null
    }

    # A census with no gathers is an instrument that stopped, not a submission of
    # zero. The distinction exists in the engine by design -- suppressed paths
    # call RecordGather before continue -- and this is where the harness reads it.
    'quadsPerGather' = {
        param($leg)
        if ($null -eq $leg.CamGathers -or $leg.CamGathers -eq 0) {
            return 'NO CENSUS: cameraGathers=0 means the instrument did not run (voxel.Stream.GPUCullStatsPeriod defaults to 0), NOT that submission was zero.'
        }
        return $null
    }
}

function Test-MetricValid {
    param($Leg, [string]$Metric)
    if (-not $MetricValidity.ContainsKey($Metric)) { return $null }
    return (& $MetricValidity[$Metric] $Leg)
}

# ==============================================================================
# SECTION 6 -- THE LEG PARSER
# ==============================================================================

function Set-Fact {
    param($Obj, [string]$Name, $Value)
    $Obj.PSObject.Properties[$Name].Value = $Value
}

function Get-LegFacts {
    param([string]$Name, $Arm)

    $path = Join-Path $SavedDir "$Name.log"
    $f = [pscustomobject]@{
        Leg=$Name; ArmId=$(if ($Arm) { $Arm.Id } else { 'unknown' }); Arm=$Arm
        Valid=$false; VoidReasons=@(); Notes=@()
        Cvars=@{}; Cmdline=@{}; Refusals=@{}; Fingerprint=@()
        FrameP50=$null; FrameP95=$null; FrameMax=$null; Frames=$null; Hitches=$null
        MarchMode=$null; MarchSource=$null; MarchMs=$null; EmitMs=$null; ScratchMs=$null
        TilesTotal=$null; TilesDrawn=$null; IndexEntries=$null; DeclinedNoPool=$null
        Settle=$null; ColdFillSec=$null; LoadedAtSettle=$null; PeakLoaded=$null
        QuadPoolCapacityQuads=$null; QuadPoolCapacityMB=$null
        ResidentQuads=$null; QuadContentMiB=$null; QuadPoolCapacityPct=$null; QuadPoolAllocFail=$null
        BrickChunks=$null; BrickResidentMiB=$null; BrickCommittedMiB=$null
        BrickEvictions=$null; BrickAllocFail=$null
        MeshMs=$null; MeshCount=$null; FillMs=$null; FillCount=$null; PackMs=$null; PackCount=$null
        WorkerMsPerChunk=$null; PacksPerSec=$null
        CamGathers=$null; CamQuadsPerGather=$null; ShadowQuadsPerGather=$null
        TotalQuadsPerGather=$null; ShadowMultiplier=$null
        SunFirst=$null; SunLast=$null; SunDrift=$null
        BuildId=$null
    }

    if (-not (Test-Path $path)) { $f.VoidReasons += "NO LOG at $path"; return $f }
    $lines = Get-Content $path

    # ---- completeness -------------------------------------------------------
    # A PARTIAL LEG IS NOT OBVIOUSLY PARTIAL. It has a plausible chunks/s, a
    # plausible hole count and a plausible everything else, because the flight
    # profile front-loads the cheap phase. Three times in one session a mid-
    # flight log was compared against finished ones and read as a slow
    # configuration. The expected window count is derived from the leg's OWN
    # command line rather than hardcoded, so changing the profile cannot make
    # this check quietly permissive.
    $applyWindows = @($lines | Select-String -SimpleMatch 'Voxel apply stages').Count

    # ---- command line -------------------------------------------------------
    $cmdLine = ($lines | Select-String -SimpleMatch 'LogInit: Command Line:' | Select-Object -First 1)
    $interval = 5.0; $expectedSec = 0.0
    if ($cmdLine) {
        foreach ($k in $FingerprintCmdline) {
            $m = [regex]::Match($cmdLine.Line, "-$k=([^\s`"]+)")
            if ($m.Success) { $f.Cmdline[$k] = $m.Groups[1].Value } else { $f.Cmdline[$k] = '(default)' }
        }
        foreach ($k in @('VoxelPerfRun','VoxelPerfPreflightSec','VoxelPerfLingerSec')) {
            $m = [regex]::Match($cmdLine.Line, "-$k=([0-9.]+)")
            if ($m.Success) { $expectedSec += [double]$m.Groups[1].Value }
        }
        $m = [regex]::Match($cmdLine.Line, '-VoxelPerfLogInterval=([0-9.]+)')
        if ($m.Success) { $interval = [double]$m.Groups[1].Value }
    } else {
        $f.VoidReasons += 'no "LogInit: Command Line:" -- pose, resolution and profile cannot be established'
    }
    if ($expectedSec -gt 0 -and $interval -gt 0) {
        $expectedWindows = [int]([math]::Floor(($expectedSec / $interval) * 0.9))
        if ($applyWindows -lt $expectedWindows) {
            $f.VoidReasons += ("INCOMPLETE: {0} 'Voxel apply stages' windows against {1} expected for a {2}s profile at {3}s intervals. The leg died or is still running." -f $applyWindows, $expectedWindows, $expectedSec, $interval)
        }
    }

    # ---- cvars --------------------------------------------------------------
    $resolved = Resolve-CvarHistory -Lines $lines
    foreach ($c in $FingerprintCvars) { $f.Cvars[$c] = Get-CvarFinal -Resolved $resolved -Name $c }
    $f.Refusals = $resolved.Refusals
    foreach ($k in $resolved.Refusals.Keys) {
        $r = $resolved.Refusals[$k][-1]
        $f.Notes += ("CVAR WRITE REFUSED: {0} with {1} was ignored ({2} still holds); value remains {3}, log line {4}" -f $k, $r.Tried, $r.Held, $r.Remains, $r.Line)
    }

    # ---- R1: does the leg prove the configuration its arm claims? -----------
    if ($Arm) {
        foreach ($k in $Arm.Defines.Keys) {
            $want = ConvertTo-CvarCanonical $Arm.Defines[$k]
            $got  = $f.Cvars[$k]
            if (-not $got) { $got = Get-CvarFinal -Resolved $resolved -Name $k }
            if (-not $got.Known) {
                $f.VoidReasons += "arm claims $k=$want but this log contains NO WRITE of it. The configuration is unproven -- 'the default says so' is the standard of evidence that failed on r.ShadowQuality."
                continue
            }
            if ($got.Value -ne $want) {
                $f.VoidReasons += ("arm claims {0}={1}; the LAST write in this log is {0}={2} at line {3}, set by {4}" -f $k, $want, $got.Value, $got.Line, $got.Setter)
            }
        }
    }

    # ---- settle -------------------------------------------------------------
    $settle = Get-SettlePoint -Lines $lines
    $f.Settle = $settle
    if (-not $settle.Settled) {
        $f.Notes += "NOT SETTLED: $($settle.Reason). Every settle-dependent metric is omitted."
    } else {
        $f.ColdFillSec = $settle.FillSeconds
        $f.LoadedAtSettle = $settle.SettleWindow.Loaded
        $f.PeakLoaded = $settle.PeakLoaded
        $f.ResidentQuads = $settle.SettleWindow.ResidentQuads
        # Quads are 8 bytes each -- the same conversion the pool's own startup
        # line uses to print its capacity in MB.
        $f.QuadContentMiB = [math]::Round(($settle.SettleWindow.ResidentQuads * 8.0) / 1048576.0, 1)
    }

    # ---- frame distribution -------------------------------------------------
    # The POST-WARMUP line, not the full-run line. They disagree -- by 3.8 ms on
    # p95 in one recorded case -- and post-warmup is the one every recorded
    # result on this project used.
    $pw = @($lines | Select-String -Pattern 'VoxelPerfRun post-warmup .*?frames=(\d+) p50=([\d.]+)ms p95=([\d.]+)ms max=([\d.]+)ms hitches=(\d+)') | Select-Object -Last 1
    if ($pw) {
        $g = $pw.Matches[0].Groups
        $f.Frames = [int]$g[1].Value; $f.FrameP50 = [double]$g[2].Value
        $f.FrameP95 = [double]$g[3].Value; $f.FrameMax = [double]$g[4].Value
        $f.Hitches = [int]$g[5].Value
    }

    # ---- marcher ------------------------------------------------------------
    # marchMs/emitMs/scratchMs are STRINGS in the format -- 'never-ran',
    # 'pending' or a number -- so a numeric-only regex silently skips the line
    # on exactly the legs where the marcher did not run, which is when you most
    # need to know.
    $mm = @($lines | Select-String -Pattern 'Voxel march: mode=(\w+) source=(\w+) stepBudget=(\d+).*?marchMs=([\w.-]+) emitMs=([\w.-]+) scratchMs=([\w.-]+).*?tiles total=(\d+) drawn=(\d+) \| indexEntries=(-?\d+)') | Select-Object -Last 1
    if ($mm) {
        $g = $mm.Matches[0].Groups
        $f.MarchMode = $g[1].Value; $f.MarchSource = $g[2].Value
        foreach ($p in @(@('MarchMs',4), @('EmitMs',5), @('ScratchMs',6))) {
            $v = $g[$p[1]].Value
            if ($v -match '^[\d.]+$') { Set-Fact $f $p[0] ([double]$v) }
            else { $f.Notes += "$($p[0]) reads '$v' -- the marcher did not produce a timing on this leg" }
        }
        $f.TilesTotal = [int]$g[7].Value; $f.TilesDrawn = [int]$g[8].Value
        $f.IndexEntries = [int]$g[9].Value
    } else {
        # Older format without source= / indexEntries=.
        $mm2 = @($lines | Select-String -Pattern 'Voxel march: mode=(\w+) stepBudget=(\d+).*?marchMs=([\w.-]+) emitMs=([\w.-]+) scratchMs=([\w.-]+).*?tiles total=(\d+) drawn=(\d+)') | Select-Object -Last 1
        if ($mm2) {
            $g = $mm2.Matches[0].Groups
            $f.MarchMode = $g[1].Value
            foreach ($p in @(@('MarchMs',3), @('EmitMs',4), @('ScratchMs',5))) {
                $v = $g[$p[1]].Value
                if ($v -match '^[\d.]+$') { Set-Fact $f $p[0] ([double]$v) }
            }
            $f.TilesTotal = [int]$g[6].Value; $f.TilesDrawn = [int]$g[7].Value
            $f.Notes += 'march stats are the pre-source= log format: the source cannot be read from this leg'
        }
    }
    $np = @($lines | Select-String -Pattern 'declined noView=\d+ noVolume=\d+ noTextures=\d+ unsupported=\d+ noPool=(\d+)') | Select-Object -Last 1
    if ($np) {
        $f.DeclinedNoPool = [long]$np.Matches[0].Groups[1].Value
        if ($f.DeclinedNoPool -gt 0) {
            # noPool means the marcher declined the frame because the brick pool
            # had never flushed or the chunk index had never uploaded. An unbound
            # SRV reads as zeros, so the alternative to declining is rendering a
            # perfectly empty world with no error at all -- the decline is the
            # instrument working, and a non-zero count means part of this leg
            # measured a marcher that was not running.
            $f.VoidReasons += "the marcher declined $($f.DeclinedNoPool) frames for noPool -- the brick pool or chunk index was not ready, so part of this leg measured a marcher that was not marching"
        }
    }

    # TWO READS, AND WHETHER THEY AGREE. The parser takes the last, but a number
    # that moves between two reads of the same settled scene is not settled.
    # The depth-gate instrument moved 0.0094% -> 0.0342% between reads of one
    # scene before it was made an average over 64 frames.
    $allMarch = @($lines | Select-String -Pattern 'Voxel march: mode=\w+.*?marchMs=([\d.]+)')
    if ($allMarch.Count -ge 2) {
        $r1 = [double]$allMarch[-2].Matches[0].Groups[1].Value
        $r2 = [double]$allMarch[-1].Matches[0].Groups[1].Value
        $drift = [math]::Abs($r2 - $r1)
        if ($drift -gt $NoiseFloorMs) {
            $f.Notes += ("marchMs moved {0:N3} -> {1:N3} between the two deferred reads ({2:N3} ms apart, over a {3:N2} ms floor). The scene was still changing; do not quote this as a settled cost." -f $r1, $r2, $drift, $NoiseFloorMs)
        } else {
            $f.Notes += ("marchMs reads agree: {0:N3} / {1:N3} (drift {2:N3} ms)" -f $r1, $r2, $drift)
        }
    } elseif ($allMarch.Count -eq 1) {
        $f.Notes += 'only one voxel.March.Stats read in this leg -- convergence between reads was not checked'
    }

    # ---- quad pool ----------------------------------------------------------
    # Capacity comes from the ONE startup line; there are no bytes on the
    # per-window pool line at all. Read the per-window line AT SETTLE, never the
    # last one: the last line of a leg is the linger phase and the first is
    # preflight, and both describe a phase rather than a configuration.
    $cap = @($lines | Select-String -Pattern 'geometry pool up, (\d+) quad capacity \(([\d.]+) MB\)') | Select-Object -First 1
    if ($cap) {
        $f.QuadPoolCapacityQuads = [long]$cap.Matches[0].Groups[1].Value
        $f.QuadPoolCapacityMB    = [double]$cap.Matches[0].Groups[2].Value
    }
    $poolLines = @($lines | Select-String -Pattern 'Voxel GPU pool: liveChunks=(\d+) highWater=(\d+).*?capacityPct=([\d.]+) allocFail=(\d+)')
    if ($poolLines.Count -gt 0) {
        $pick = $null
        if ($settle.Settled) {
            $after = @($poolLines | Where-Object { $_.LineNumber -ge $settle.SettleLine })
            if ($after.Count -gt 0) { $pick = $after[0] }
        }
        if ($pick) {
            $g = $pick.Matches[0].Groups
            $f.QuadPoolCapacityPct = [double]$g[3].Value
            $f.QuadPoolAllocFail   = [long]$g[4].Value
        } else {
            $f.Notes += 'quad pool line exists but none falls at or after settle -- pool figures omitted'
        }
    }

    # ---- brick pool ---------------------------------------------------------
    # voxel.Brick.Stats is a console COMMAND, not a periodic line, and its
    # continuation lines carry no log prefix at all.
    $bp = @($lines | Select-String -Pattern 'voxel\.Brick\.Stats: (\d+) chunks resident of \d+; resident ([\d.]+) MiB, committed ([\d.]+) MiB') | Select-Object -Last 1
    if ($bp) {
        $g = $bp.Matches[0].Groups
        $f.BrickChunks = [int]$g[1].Value
        $f.BrickResidentMiB = [double]$g[2].Value
        $f.BrickCommittedMiB = [double]$g[3].Value
        if ($settle.Settled -and $bp.LineNumber -lt $settle.SettleLine) {
            $f.Notes += 'voxel.Brick.Stats fired BEFORE settle -- the brick figures describe a partly-filled pool and are omitted from the table'
            $f.BrickChunks = $null; $f.BrickResidentMiB = $null; $f.BrickCommittedMiB = $null
        }
    } else {
        $f.Notes += 'no voxel.Brick.Stats output -- it is a console command and must be deferred into the leg'
    }
    $ev = @($lines | Select-String -Pattern 'added \d+(?: \(gpu \d+, cpu \d+\))?, evictions (\d+)(?: \(\d+ by distance\))?, allocFail (\d+)') | Select-Object -Last 1
    if ($ev) {
        $f.BrickEvictions = [long]$ev.Matches[0].Groups[1].Value
        $f.BrickAllocFail = [long]$ev.Matches[0].Groups[2].Value
    }

    # ---- worker terms: PARSED PER TERM, NEVER FROM THE PRINTED TOTAL --------
    # R4. The printed "THIS ARM'S TOTAL" sums three per-chunk means whose counts
    # differ by 870x on the suppression arm. It read 1.127 ms/chunk where
    # total/count is 0.389 -- a 3x inflation that reversed the verdict, and it
    # failed in the SAFE direction, which is the only reason it was caught. The
    # instrument's own header states the per-term rule correctly and then the
    # total ignores it one line later.
    foreach ($term in @('mesh','fill','pack')) {
        $t = @($lines | Select-String -Pattern ("^\s+{0}\s+(\d+) chunks\s+([\d.]+) ms" -f $term)) | Select-Object -Last 1
        if ($t) {
            $g = $t.Matches[0].Groups
            $cap = $term.Substring(0,1).ToUpper() + $term.Substring(1)
            Set-Fact $f "${cap}Count" ([long]$g[1].Value)
            Set-Fact $f "${cap}Ms"    ([double]$g[2].Value)
        }
    }
    $totalMs = 0.0
    foreach ($v in @($f.MeshMs, $f.FillMs, $f.PackMs)) { if ($null -ne $v) { $totalMs += $v } }
    # THE DENOMINATOR IS THE PACK COUNT. Every chunk that reaches the pool is
    # packed exactly once, whereas mesh and fill are mutually exclusive across
    # arms -- and the mesh term on a suppression arm covers 96 game-thread edit
    # chunks that are NOT the arm. Dividing by pack count is total-over-count for
    # the quantity the axis is about: worker cost per chunk delivered.
    if ($null -ne $f.PackCount -and $f.PackCount -gt 0) {
        $f.WorkerMsPerChunk = [math]::Round($totalMs / $f.PackCount, 4)
    }
    $ps = @($lines | Select-String -Pattern 'fill span: (\d+) packs over ([\d.]+) s = (\d+) packs/s') | Select-Object -Last 1
    if ($ps) { $f.PacksPerSec = [long]$ps.Matches[0].Groups[3].Value }

    # ---- geometry census ----------------------------------------------------
    # TOTAL-OVER-COUNT, DIFFERENCED ACROSS THE SETTLED WINDOW. Neither the
    # per-window figure (a statement about one window, and the phases differ
    # completely) nor the raw cumulative (which includes the empty pre-stream
    # pool) is the right number. The difference between the cumulative counters
    # at settle and at the end is quads-over-gathers for the settled portion
    # only, which is the thing the axis is about.
    $rxCen = [regex]'census\[window\]: cameraGathers=\d+ shadowGathers=\d+.*?cumulative: camGathers=(\d+) shadowGathers=(\d+) camQuads=(\d+) shadowQuads=(\d+)'
    $cens = @()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $m = $rxCen.Match($lines[$i])
        if ($m.Success) {
            $cens += [pscustomobject]@{
                LineNo=$i+1; CamG=[double]$m.Groups[1].Value; ShG=[double]$m.Groups[2].Value
                CamQ=[double]$m.Groups[3].Value; ShQ=[double]$m.Groups[4].Value
            }
        }
    }
    if ($cens.Count -ge 2) {
        $start = $cens[0]
        if ($settle.Settled) {
            $after = @($cens | Where-Object { $_.LineNo -ge $settle.SettleLine })
            if ($after.Count -ge 2) { $start = $after[0] }
        }
        $end = $cens[-1]
        $dG = $end.CamG - $start.CamG
        if ($dG -gt 0) {
            $f.CamGathers = [long]$dG
            $f.CamQuadsPerGather    = [long][math]::Round(($end.CamQ - $start.CamQ) / $dG)
            $f.ShadowQuadsPerGather = [long][math]::Round(($end.ShQ - $start.ShQ) / $dG)
            $f.TotalQuadsPerGather  = $f.CamQuadsPerGather + $f.ShadowQuadsPerGather
            if ($f.CamQuadsPerGather -gt 0) {
                $f.ShadowMultiplier = [math]::Round($f.TotalQuadsPerGather / [double]$f.CamQuadsPerGather, 3)
            }
        } else {
            $f.Notes += 'census counters did not advance after settle -- no gathers to divide by'
        }
    }

    # ---- sun ----------------------------------------------------------------
    # A moving sun inside a leg makes the leg's own halves describe different
    # lighting: the distribution accumulates over one sun and the deferred
    # capture fires under another. FROZEN at an unstated hour is no better --
    # two frozen legs at different hours are as incomparable as a frozen and a
    # moving one -- so the pose is recorded as well as the drift.
    $skyLines = @($lines | Select-String -SimpleMatch 'Voxel sky (')
    if ($skyLines.Count -gt 0) {
        $fm = [regex]::Match($skyLines[0].Line,  'tod=(\d{2}:\d{2}) sunAlt=(-?[\d.]+)')
        $lm = [regex]::Match($skyLines[-1].Line, 'tod=(\d{2}:\d{2}) sunAlt=(-?[\d.]+)')
        if ($fm.Success -and $lm.Success) {
            $f.SunFirst = "$($fm.Groups[1].Value)/$($fm.Groups[2].Value)"
            $f.SunLast  = "$($lm.Groups[1].Value)/$($lm.Groups[2].Value)"
            $f.SunDrift = [math]::Round([math]::Abs([double]$lm.Groups[2].Value - [double]$fm.Groups[2].Value), 2)
            if ($f.SunDrift -ge 0.01) {
                $f.VoidReasons += "THE SUN MOVED $($f.SunDrift) deg during this leg -- its distribution and its capture describe different lighting"
            }
        }
    } else {
        $f.Notes += 'no "Voxel sky" line: the leg predates the clock instrument. That is NOT the same as a frozen sun, and the two must not be conflated.'
    }

    # ---- fingerprint --------------------------------------------------------
    # BUILT FROM THE LEG'S OWN LOG, DELIBERATELY. Reading the ini files on disk
    # would describe the machine now, not the machine the leg ran on -- and
    # DefaultEngine.ini changed twice during 2026-08-19, once mid-session.
    $fp = New-Object System.Collections.Generic.List[string]
    foreach ($c in $FingerprintCvars) {
        $v = $f.Cvars[$c]
        if ($v.Known) { $fp.Add("$c=$($v.Value)") } else { $fp.Add("$c=<unwritten>") }
    }
    foreach ($k in $FingerprintCmdline) {
        $val = '(absent)'
        if ($f.Cmdline.ContainsKey($k)) { $val = $f.Cmdline[$k] }
        $fp.Add("$k=$val")
    }
    # The sidecar carries the one thing the log cannot: WHICH BINARY RAN. The
    # engine's own "Compiled" line is the ENGINE's build date, not the game
    # module's, so it cannot answer this.
    $side = Join-Path $SavedDir "$Name.fingerprint.json"
    if (Test-Path $side) {
        $sc = Get-Content $side -Raw | ConvertFrom-Json
        $f.BuildId = $sc.BuildId
        $fp.Add("build=$($sc.BuildId)")
    } else {
        $f.BuildId = 'UNRECORDED'
        $fp.Add('build=UNRECORDED')
        $f.Notes += 'BUILD UNRECORDED: this leg was not launched by this driver, so the binary it ran cannot be established. Deltas within a matched set still stand; the build is unverified.'
    }
    $f.Fingerprint = $fp

    $f.Valid = ($f.VoidReasons.Count -eq 0)
    return $f
}

# ==============================================================================
# SECTION 7 -- R2: THE TABLE REFUSAL
# ==============================================================================
#
# Two legs go in one table only if their fingerprints differ EXCLUSIVELY in keys
# the axis declared as its independent variable. Anything else is named, and the
# table is refused rather than annotated.
#
# This is the rule that stops 18.99 ms and 34.72 ms sitting in one column. Both
# are real. They describe different games.

function Compare-Fingerprints {
    param($LegA, $LegB, [string[]]$AllowedToVary)
    $a = @{}; foreach ($p in $LegA.Fingerprint) { $kv = $p -split '=', 2; $a[$kv[0]] = $kv[1] }
    $b = @{}; foreach ($p in $LegB.Fingerprint) { $kv = $p -split '=', 2; $b[$kv[0]] = $kv[1] }
    $diffs = @()
    foreach ($k in $a.Keys) {
        if ($a[$k] -ne $b[$k]) {
            if ($AllowedToVary -contains $k) { continue }
            $diffs += [pscustomobject]@{ Key=$k; A=$a[$k]; B=$b[$k] }
        }
    }
    return $diffs
}

# ==============================================================================
# SECTION 8 -- REPLICATES, SPREAD, AND THE NOISE FLOOR
# ==============================================================================
#
# A delta is a result only if it clears the rig's own within-config spread. That
# spread is 0.18 ms (~1%), measured over four A0-equivalent legs. It is not a
# guess and not a tolerance to widen. Below it this driver reports UNRESOLVED
# and refuses to print a direction -- a small number with a confident sign is
# exactly the kind of finding that has to be withdrawn later, and a withdrawn
# number is worth less than a smaller one that holds.

function Measure-Replicates {
    param([double[]]$Values)
    if (-not $Values -or $Values.Count -eq 0) { return $null }
    $s = $Values | Measure-Object -Average -Minimum -Maximum
    return [pscustomobject]@{
        N=$Values.Count; Mean=[math]::Round($s.Average,4)
        Spread=[math]::Round($s.Maximum - $s.Minimum,4); Min=$s.Minimum; Max=$s.Maximum
    }
}

function Compare-Arms {
    param($ControlStats, $ArmStats, [double]$Floor, [string]$Unit = 'ms')

    if ($null -eq $ControlStats -or $null -eq $ArmStats) {
        return [pscustomobject]@{ Verdict='UNMEASURED'; Text='one side has no valid legs carrying this metric' }
    }
    if ($ControlStats.N -lt 2 -or $ArmStats.N -lt 2) {
        $note = "SINGLE LEG on at least one side (n=$($ControlStats.N)/$($ArmStats.N)): no spread, so nothing can be said about whether the delta clears the rig's noise. "
    } else { $note = '' }

    $delta = $ArmStats.Mean - $ControlStats.Mean
    $worst = [math]::Max($ControlStats.Spread, $ArmStats.Spread)
    # BOTH gates, not the friendlier one: the rig's calibrated floor AND twice
    # the worst spread these particular legs actually produced, which can be
    # worse than the rig floor on a bad night.
    $bar = [math]::Max($Floor, 2 * $worst)

    if ([math]::Abs($delta) -lt $bar) {
        return [pscustomobject]@{
            Verdict='UNRESOLVED'; Delta=[math]::Round($delta,4); Bar=[math]::Round($bar,4)
            Text=("{0}delta {1:N3} {2} does not clear the bar of {3:N3} {2} (max of rig floor {4:N2} and 2x worst within-arm spread {5:N3}). NOT A RESULT." -f $note, $delta, $Unit, $bar, $Floor, $worst)
        }
    }
    $ratio = $null
    if ($ControlStats.Mean -ne 0) { $ratio = [math]::Round($ArmStats.Mean / $ControlStats.Mean, 3) }
    return [pscustomobject]@{
        Verdict='RESOLVED'; Delta=[math]::Round($delta,3); Ratio=$ratio; Bar=[math]::Round($bar,4)
        Text=("{0}{1:N3} -> {2:N3} {3} ({4:+0.000;-0.000}, x{5}) | n={6}/{7} | spreads {8:N3}/{9:N3} | bar {10:N3}" -f
              $note, $ControlStats.Mean, $ArmStats.Mean, $Unit, $delta, $ratio,
              $ControlStats.N, $ArmStats.N, $ControlStats.Spread, $ArmStats.Spread, $bar)
    }
}

# ==============================================================================
# SECTION 9 -- THE IDENTITY CHECK, CARRIED ON THE FRAME PAIR
# ==============================================================================
#
# mode2frame - mode0frame should equal marchMs + emitMs + scratchMs. It closed
# to 0.137 ms (2.5%) against the 0.18 ms floor, which retired a whole class of
# doubt -- hidden barriers, un-timed clears, pass setup, descriptor churn --
# without having to enumerate them. If it stops closing, something is being paid
# that no bracket names, and the frame-time row is not trustworthy however clean
# it looks. So it is evaluated every time the frame axis runs, not once.

function Test-MarchIdentity {
    param($RasterStats, $MarchLeg, [double]$Floor)
    if ($null -eq $RasterStats -or $null -eq $MarchLeg) { return $null }
    if ($null -eq $MarchLeg.MarchMs -or $null -eq $MarchLeg.EmitMs -or $null -eq $MarchLeg.ScratchMs) {
        return [pscustomobject]@{ Closed=$null; Text='the marcher leg carries no bracket timings -- the identity cannot be evaluated' }
    }
    if ($null -eq $MarchLeg.FrameP50) {
        return [pscustomobject]@{ Closed=$null; Text='the marcher leg carries no frame distribution' }
    }
    $measured  = $MarchLeg.FrameP50 - $RasterStats.Mean
    $bracketed = $MarchLeg.MarchMs + $MarchLeg.EmitMs + $MarchLeg.ScratchMs
    $residual  = $measured - $bracketed
    return [pscustomobject]@{
        Closed = ([math]::Abs($residual) -lt $Floor)
        Measured=[math]::Round($measured,3); Bracketed=[math]::Round($bracketed,3)
        Residual=[math]::Round($residual,3)
        Text=("measured delta {0:N3} ms against bracketed {1:N3} ms (march {2:N3} + emit {3:N3} + scratch {4:N3}); residual {5:N3} ms against a {6:N2} ms floor" -f
              $measured, $bracketed, $MarchLeg.MarchMs, $MarchLeg.EmitMs, $MarchLeg.ScratchMs, $residual, $Floor)
    }
}

# ==============================================================================
# SECTION 10 -- PREFLIGHT: THE SHADER TREE AND THE BUILT BINARY MUST AGREE
# ==============================================================================
#
# THE RECORDED RULE WAS "a half-finished .usf is live input to the next leg,
# unlike C++ which is inert until built." THE INVERSE IS EQUALLY FATAL and cost
# a leg on 2026-08-19: a COMPLETE .usf whose matching C++ was deliberately not
# built. p0-sh-geom-r1 exited after 9 s of an expected 250 s on a global shader
# compile error at boot. The invariant is not "shaders compile" -- it is THE
# SHADER TREE AND THE BUILT BINARY AGREE.
#
# This cannot be proved from timestamps alone, and this driver does not build.
# What it CAN do is refuse to start a 40-minute serialized run when a .usf is
# newer than every game module binary, which is the observable signature of that
# exact failure, and say plainly what it is refusing.

function Get-BuildFingerprint {
    $binDir = Join-Path $RepoRoot 'ue-project\Binaries\Win64'
    $mods = @()
    if (Test-Path $binDir) {
        $mods = @(Get-ChildItem $binDir -Filter 'UnrealEditor-Voxel*.dll' -EA SilentlyContinue)
    }
    if ($mods.Count -eq 0) {
        return [pscustomobject]@{ BuildId='NO-MODULES-FOUND'; Newest=$null; Modules=@() }
    }
    $newest = ($mods | Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum
    $sig = ($mods | Sort-Object Name | ForEach-Object { "$($_.Name):$($_.Length):$($_.LastWriteTimeUtc.Ticks)" }) -join '|'
    $md5 = [System.Security.Cryptography.MD5]::Create()
    $hash = ($md5.ComputeHash([Text.Encoding]::UTF8.GetBytes($sig)) | ForEach-Object { $_.ToString('x2') }) -join ''
    return [pscustomobject]@{
        BuildId = $hash.Substring(0,12); Newest = $newest
        Modules = @($mods | ForEach-Object { $_.Name })
    }
}

function Test-ShaderTreeAgreesWithBinary {
    param($Build)
    $shaderDirs = @(
        (Join-Path $RepoRoot 'ue-project\Shaders'),
        (Join-Path $RepoRoot 'voxel-core\shaders')
    )
    $newestShader = $null; $newestName = $null
    foreach ($d in $shaderDirs) {
        if (-not (Test-Path $d)) { continue }
        foreach ($s in (Get-ChildItem $d -Recurse -Include *.usf,*.ush -EA SilentlyContinue)) {
            if ($null -eq $newestShader -or $s.LastWriteTimeUtc -gt $newestShader) {
                $newestShader = $s.LastWriteTimeUtc; $newestName = $s.FullName
            }
        }
    }
    if ($null -eq $newestShader -or $null -eq $Build.Newest) {
        return [pscustomobject]@{ Agrees=$true; Text='no shader tree or no modules found -- check skipped' }
    }
    if ($newestShader -gt $Build.Newest) {
        return [pscustomobject]@{
            Agrees=$false
            Text=("SHADER TREE IS AHEAD OF THE BUILT BINARY. {0} was written {1:u}, newer than every game module ({2:u}). A leg in this state dies at boot on a global shader compile error -- it happened on 2026-08-19 and cost a 250 s leg after 9 s. BUILD BEFORE RUNNING, then re-run this driver. (This driver does not build.)" -f $newestName, $newestShader, $Build.Newest)
        }
    }
    return [pscustomobject]@{ Agrees=$true; Text=("shader tree ({0:u}) is not newer than the built modules ({1:u})" -f $newestShader, $Build.Newest) }
}

# ==============================================================================
# SECTION 11 -- PLANNING AND EXECUTION
# ==============================================================================

function Get-LegPlan {
    param([string[]]$Axes, [int]$Reps)

    $wanted = $ArmCatalogue
    if ($Axes -notcontains 'all') { $wanted = @($ArmCatalogue | Where-Object { $Axes -contains $_.Axis }) }

    $plan = @()
    # INTERLEAVED, NOT BLOCKED. Arm-then-arm ordering lets any slow drift of the
    # box -- thermals, background work, a cache warming -- land entirely on one
    # side of a pair and read as a configuration difference. Alternating puts
    # that drift into the within-arm spread instead, where it is visible and
    # where the noise bar accounts for it.
    for ($r = 1; $r -le $Reps; $r++) {
        foreach ($arm in $wanted) {
            $plan += [pscustomobject]@{
                LegName = ("fc-{0}-r{1}" -f $arm.Id, $r)
                Arm = $arm; Replicate = $r
            }
        }
    }
    return $plan
}

function Get-LegArgs {
    param($Entry)
    return @{
        LogName = $Entry.LegName
        Cvars = $Entry.Arm.Cvars
        Width = $Pose.Width; Height = $Pose.Height
        RunSec = $Pose.RunSec; PreflightSec = $Pose.PreflightSec; LingerSec = $Pose.LingerSec
        # The deferred ProfileGPU must land INSIDE the flight: preflight plus
        # roughly half the run. -ExecCmds fires at startup and would otherwise
        # profile frame 1 of an empty world, which it reports as a successful
        # capture. That has cost three separate runs on this project.
        CaptureAt = ($Pose.PreflightSec + [int]($Pose.RunSec / 2))
        SpawnAt = $Pose.SpawnAt; Flight = $Pose.Flight
    }
}

function Write-Plan {
    param($Plan, $Build, $ShaderCheck)

    Write-Host ''
    Write-Host '=== THE PLAN ===' -ForegroundColor Cyan
    Write-Host ("{0} legs, serialized, one editor at a time." -f $Plan.Count)
    $secs = $Plan.Count * ($Pose.PreflightSec + $Pose.RunSec + $Pose.LingerSec)
    Write-Host ("Wall time at the profile below: ~{0:N0} min, plus process start per leg." -f ($secs / 60.0))
    Write-Host ("Pose: spawn {0} m, flight {1}, {2}x{3}, sun pinned 12:00 / 03-20 / timeScale 0." -f $Pose.SpawnAt, $Pose.Flight, $Pose.Width, $Pose.Height)
    Write-Host ("Profile: {0}s preflight + {1}s run + {2}s linger, census every {3}s." -f $Pose.PreflightSec, $Pose.RunSec, $Pose.LingerSec, $Pose.LogInterval)
    Write-Host ("Build fingerprint: {0} ({1} modules)" -f $Build.BuildId, $Build.Modules.Count)
    if ($ShaderCheck.Agrees) {
        Write-Host ("Shader/binary agreement: OK -- {0}" -f $ShaderCheck.Text) -ForegroundColor Green
    } else {
        Write-Host ("Shader/binary agreement: REFUSED -- {0}" -f $ShaderCheck.Text) -ForegroundColor Red
    }
    Write-Host ''
    foreach ($e in $Plan) {
        Write-Host ("  {0,-24} {1}" -f $e.LegName, $e.Arm.Label)
        Write-Host ("  {0,-24}   cvars: {1}" -f '', $e.Arm.Cvars) -ForegroundColor DarkGray
        $defs = ($e.Arm.Defines.Keys | ForEach-Object { "$_=$($e.Arm.Defines[$_])" }) -join ', '
        Write-Host ("  {0,-24}   MUST PROVE (or VOID): {1}" -f '', $defs) -ForegroundColor DarkGray
    }
    Write-Host ''
    Write-Host 'Each leg is then checked for:' -ForegroundColor Cyan
    Write-Host '  R1  the LAST write of every defining cvar, and what set it (four log shapes, incl. refusals)'
    Write-Host '  R2  a config fingerprint; two legs with differing fingerprints are refused a shared table'
    Write-Host '  R3  metric meaning per arm; anything arm-dependent is labelled in the cell or omitted'
    Write-Host '  R4  totals recomputed as total-over-count; no printed TOTAL is ever read'
    Write-Host '  R5  settle = first idle window AT PEAK loaded, with work seen before and windows after'
    Write-Host '  +   completeness against the leg''s own profile, and a frozen sun'
    Write-Host ''
    Write-Host 'Nothing has been launched. Re-run with -Execute to spend these legs.' -ForegroundColor Yellow
}

function Invoke-Plan {
    param($Plan, $Build)

    $running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -EA SilentlyContinue)
    if ($running.Count -gt 0) {
        throw "REFUSING TO START: an editor is already running -- $(($running | ForEach-Object { "PID $($_.Id)" }) -join ', '). One editor per box."
    }

    $done = @()
    foreach ($e in $Plan) {
        Write-Host ("RUN {0} -- {1}" -f $e.LegName, $e.Arm.Label) -ForegroundColor Cyan
        $splat = Get-LegArgs -Entry $e
        $res = & $Runner @splat
        $ok = @($res)[-1]
        # THE SIDECAR IS WRITTEN AFTER THE LEG, NOT BEFORE, so a leg that never
        # finished cannot leave behind a fingerprint claiming it ran this build.
        if ($ok) {
            $side = Join-Path $SavedDir "$($e.LegName).fingerprint.json"
            [pscustomobject]@{
                BuildId=$Build.BuildId; Modules=$Build.Modules
                NewestModuleUtc=$Build.Newest; ArmId=$e.Arm.Id; Replicate=$e.Replicate
                RanUtc=(Get-Date).ToUniversalTime().ToString('u')
            } | ConvertTo-Json | Set-Content -Path $side -Encoding UTF8
        } else {
            Write-Host ("  {0}: the runner reported VOID -- it will be parsed and reported as void, not silently dropped." -f $e.LegName) -ForegroundColor Yellow
        }
        $done += $e
    }
    return $done
}

# ==============================================================================
# SECTION 12 -- THE REPORT
# ==============================================================================
#
# THREE SECTIONS, UNAMBIGUOUSLY SEPARATED, because the owner has to be able to
# tell at a glance which numbers he can repeat to someone else:
#
#   MEASURED    a number from a leg, with its arm, its control, its replicate
#               count and its spread.
#   PROJECTED   arithmetic on measured inputs, WITH THE INPUTS NAMED. Never
#               presented in the same table as a measurement.
#   UNMEASURED  things the plan asserts that nobody has run. These are listed
#               explicitly rather than omitted, because an omission reads as an
#               oversight and a listed gap reads as a boundary.

# Things the plan rests on that no leg has established. Each carries WHY, so it
# can be struck off by name when a leg finally covers it.
$UnmeasuredClaims = @(
    [pscustomobject]@{
        Claim='The marcher is faster than the raster path.'
        Status='FALSE TODAY, and stated as such rather than projected past.'
        Why='At the last matched pair the marcher ADDED 5.4 ms at p50 (19.72 -> 25.23). It cannot win until hierarchical empty-space skipping lands (P3-B2, in progress). Every marcher cost so far is a DENSE walk: level 0 only, no cone LOD, no ring transitions, no mip pyramid.'
    }
    [pscustomobject]@{
        Claim='The 9.19x empty-space skip ratio, on which the whole frame-rate case rests.'
        Status='NEVER MEASURED ON THE REAL STRUCTURE.'
        Why='It was measured against a two-level mip over the FLAT 512-cubed occupancy volume -- different structure, different cell sizes, different restart behaviour. The brick pyramid it is quoted for has never been walked with skipping at all. The old cost model 0.13 + steps x 5.9 us is dead for the same reason.'
    }
    [pscustomobject]@{
        Claim='Geometry submitted goes to zero in a shipping configuration.'
        Status='MEASURABLE ONLY AS AN EMULATION.'
        Why='The quad path is not retired -- that is P4. The geometry arm suppresses submission with voxel.Stream.GPUCullDebugDrawNothing 3, which is the Phase 4 SHAPE on today''s binary, not the shipped state. The pool is still resident and still costs its VRAM.'
    }
    [pscustomobject]@{
        Claim='These frame times describe the game players will run.'
        Status='NO. They describe LOW settings.'
        Why='A stale sg.* preset (ResolutionQuality/ViewDistance/PostProcess/Effects all 0) has been in force for every leg this project has ever taken: r.ViewDistanceScale 0.4, MaterialQualityLevel Low, DetailMode 0, VolumetricFog 0, AmbientOcclusionLevels 0. Every A/B survives this -- both arms shared it -- but no ABSOLUTE number does. Raising it would invalidate comparability with every existing leg, so it is a decision for the owner, not a side effect.'
    }
    [pscustomobject]@{
        Claim='The marcher''s frame-time tail is acceptable.'
        Status='UNKNOWN, NOT FINE.'
        Why='p95 disagreed with itself by 3.8 ms inside one leg and moved 28.31 -> 43.33 across a pair whose hitch count went DOWN and whose frame counts differed by 22%. The tail needs its own arm: pinned pose, matched frame counts, hitch attribution.'
    }
    [pscustomobject]@{
        Claim='The 4 km cascade marches.'
        Status='UNMEASURED. Everything is level 0.'
        Why='The chunk index is level-0 only by construction (kIndexedLevel = 0). R0 spans 0-128 m. No number here describes a ray crossing a ring boundary, and the residency histogram shows L0 is only 19.2% of resident chunks -- though it is most of what a camera at this pose actually sees.'
    }
    [pscustomobject]@{
        Claim='Loading improves end to end.'
        Status='PARTIALLY MEASURED. Worker cost only.'
        Why='Phase 5 measured 2.49x cheaper worker time per chunk (0.969 -> 0.389 ms). That is worker-thread cost, NOT chunks/s and NOT wall-clock cold fill. Converting it into a streaming rate needs packs/s across a matched pair, and `loaded=` cannot cross the suppression arm at all.'
    }
)

function New-Report {
    param($LegFacts, $Rows, $Refusals, $Identity, $Build, [string]$Path)

    $sb = New-Object System.Text.StringBuilder
    $w = { param($s) [void]$sb.AppendLine($s) }

    & $w "# Final comparison: quad raster path vs marcher"
    & $w ""
    & $w "**Generated:** $Tag by ``tools/voxel-final-comparison.ps1``  "
    & $w "**Build fingerprint:** ``$($Build.BuildId)``  "
    & $w "**Method:** ``docs/final-comparison-method.md`` -- what every number means and what it does not."
    & $w ""
    & $w "Every figure below is one of three things and is never silently mixed:"
    & $w "**MEASURED** (a number from a leg, with arm, control, replicate count and spread),"
    & $w "**PROJECTED** (arithmetic on measured inputs, inputs named), or"
    & $w "**UNMEASURED** (asserted by the plan, run by nobody)."
    & $w ""

    # ---- legs ---------------------------------------------------------------
    & $w "## Legs"
    & $w ""
    & $w "| leg | arm | verdict | settle | sun | frames |"
    & $w "|---|---|---|---|---|---|"
    foreach ($l in $LegFacts) {
        $verdict = if ($l.Valid) { 'VALID' } else { 'VOID' }
        $settle = 'not settled'
        if ($l.Settle -and $l.Settle.Settled) { $settle = ("{0:N0} s, loaded {1:N0} (peak)" -f $l.Settle.FillSeconds, $l.Settle.PeakLoaded) }
        $sun = if ($null -ne $l.SunDrift) { if ($l.SunDrift -lt 0.01) { "FROZEN $($l.SunFirst)" } else { "MOVED $($l.SunDrift) deg" } } else { 'no sky log' }
        $fr = if ($null -ne $l.Frames) { "{0:N0}" -f $l.Frames } else { '--' }
        & $w ("| ``{0}`` | {1} | **{2}** | {3} | {4} | {5} |" -f $l.Leg, $l.ArmId, $verdict, $settle, $sun, $fr)
    }
    & $w ""
    $voided = @($LegFacts | Where-Object { -not $_.Valid })
    if ($voided.Count -gt 0) {
        & $w "### Void legs, and why"
        & $w ""
        & $w "A leg that cannot prove its own configuration is void, not reported. These are listed rather than dropped, because a dropped leg is indistinguishable from a leg that was never run."
        & $w ""
        foreach ($l in $voided) {
            & $w "**``$($l.Leg)``**"
            foreach ($r in $l.VoidReasons) { & $w "- $r" }
            & $w ""
        }
    }
    $noted = @($LegFacts | Where-Object { $_.Notes.Count -gt 0 })
    if ($noted.Count -gt 0) {
        & $w "### Leg notes"
        & $w ""
        foreach ($l in $noted) {
            foreach ($n in $l.Notes) { & $w "- ``$($l.Leg)``: $n" }
        }
        & $w ""
    }

    # ---- refused tables -----------------------------------------------------
    if ($Refusals.Count -gt 0) {
        & $w "## REFUSED COMPARISONS"
        & $w ""
        & $w "These pairs were not put in a table. Their legs differ in configuration beyond the axis's own variable, so the difference between them is not attributable to the axis. Both sides may be perfectly real numbers -- ``18.99 ms`` and ``34.72 ms`` both were, and they described different games."
        & $w ""
        foreach ($r in $Refusals) {
            & $w "**$($r.Axis)**: ``$($r.LegA)`` vs ``$($r.LegB)`` differ in:"
            foreach ($d in $r.Diffs) { & $w "- ``$($d.Key)``: ``$($d.A)`` vs ``$($d.B)``" }
            & $w ""
        }
    }

    # ---- measured -----------------------------------------------------------
    & $w "## MEASURED"
    & $w ""
    & $w "| axis | metric | old (control) | new (arm) | result | n / spread | caveat attached to this figure |"
    & $w "|---|---|---|---|---|---|---|"
    foreach ($row in $Rows) {
        & $w ("| {0} | {1} | {2} | {3} | {4} | {5} | {6} |" -f
              $row.Axis, $row.Metric, $row.Old, $row.New, $row.Result, $row.Replicates, $row.Caveat)
    }
    & $w ""
    if ($Identity) {
        & $w "### The identity check"
        & $w ""
        & $w "``mode2frame - mode0frame`` must equal ``marchMs + emitMs + scratchMs``. If it does not, something is being paid that no bracket here names, and the frame-time row above is not trustworthy however clean it looks."
        & $w ""
        $verdict = if ($Identity.Closed -eq $true) { '**CLOSES**' } elseif ($Identity.Closed -eq $false) { '**DOES NOT CLOSE**' } else { '**not evaluable**' }
        & $w "$verdict -- $($Identity.Text)"
        & $w ""
    }

    # ---- unmeasured ---------------------------------------------------------
    & $w "## UNMEASURED"
    & $w ""
    & $w "The plan rests on each of these and no leg has established any of them. They are listed by name so they can be struck off individually, rather than omitted -- an omission reads as an oversight; a listed gap reads as a boundary."
    & $w ""
    foreach ($u in $UnmeasuredClaims) {
        & $w "**$($u.Claim)**  "
        & $w "*$($u.Status)*  "
        & $w "$($u.Why)"
        & $w ""
    }

    Set-Content -Path $Path -Value $sb.ToString() -Encoding UTF8
    Write-Host "Report written: $Path" -ForegroundColor Green
}

# ==============================================================================
# SECTION 13 -- BUILDING THE ROWS
# ==============================================================================

function Get-ArmLegs {
    param($LegFacts, [string]$ArmId)
    return @($LegFacts | Where-Object { $_.ArmId -eq $ArmId -and $_.Valid })
}

function Format-Cell {
    param($Value, [string]$Fmt = 'N3', $Invalid = $null)
    if ($null -ne $Invalid) { return "n/a" }
    if ($null -eq $Value) { return '--' }
    return ("{0:$Fmt}" -f $Value)
}

function Build-Rows {
    param($LegFacts, [string[]]$Axes)

    $rows = @(); $refusals = @(); $identity = $null

    function Pair-Check {
        param($CtlLegs, $ArmLegs, [string]$AxisName, [string[]]$Varies)
        $bad = @()
        foreach ($c in $CtlLegs) {
            foreach ($a in $ArmLegs) {
                $d = Compare-Fingerprints -LegA $c -LegB $a -AllowedToVary $Varies
                if ($d.Count -gt 0) {
                    $bad += [pscustomobject]@{ Axis=$AxisName; LegA=$c.Leg; LegB=$a.Leg; Diffs=$d }
                }
            }
        }
        return $bad
    }

    # ---- frame --------------------------------------------------------------
    if ($Axes -contains 'all' -or $Axes -contains 'frame') {
        $ctl = Get-ArmLegs $LegFacts 'frame-raster'
        $arm = Get-ArmLegs $LegFacts 'frame-march'
        $varies = @('voxel.March','voxel.March.Source')
        $bad = Pair-Check $ctl $arm 'frame time' $varies
        if ($bad.Count -gt 0) {
            $refusals += $bad
        } elseif ($ctl.Count -gt 0 -and $arm.Count -gt 0) {
            $cs = Measure-Replicates @($ctl | Where-Object { $null -ne $_.FrameP50 } | ForEach-Object { $_.FrameP50 })
            $as = Measure-Replicates @($arm | Where-Object { $null -ne $_.FrameP50 } | ForEach-Object { $_.FrameP50 })
            $cmp = Compare-Arms -ControlStats $cs -ArmStats $as -Floor $NoiseFloorMs -Unit 'ms'
            $marchCaveat = ''
            $first = @($arm)[0]
            $mv = Test-MetricValid -Leg $first -Metric 'marchMs'
            if ($mv) { $marchCaveat = $mv }
            $rows += [pscustomobject]@{
                Axis='frame time'; Metric='p50 frame (post-warmup)'
                Old=(Format-Cell $cs.Mean 'N2'); New=(Format-Cell $as.Mean 'N2')
                Result=$cmp.Text
                Replicates=("n={0}/{1}, spread {2:N3}/{3:N3} ms" -f $cs.N, $as.N, $cs.Spread, $as.Spread)
                Caveat=$marchCaveat
            }
            # p95 and hitches go in labelled, because leaving a bad metric out
            # entirely invites someone to go and read it raw from the log.
            $p95c = Test-MetricValid -Leg (@($arm)[0]) -Metric 'frameP95'
            $c95 = Measure-Replicates @($ctl | Where-Object { $null -ne $_.FrameP95 } | ForEach-Object { $_.FrameP95 })
            $a95 = Measure-Replicates @($arm | Where-Object { $null -ne $_.FrameP95 } | ForEach-Object { $_.FrameP95 })
            $rows += [pscustomobject]@{
                Axis='frame time'; Metric='p95 frame'
                Old=(Format-Cell $c95.Mean 'N2'); New=(Format-Cell $a95.Mean 'N2')
                Result='not compared'; Replicates=("n={0}/{1}" -f $c95.N, $a95.N); Caveat=$p95c
            }
            foreach ($side in @(@('old',$ctl), @('new',$arm))) {
                $h = Test-MetricValid -Leg (@($side[1])[0]) -Metric 'hitches'
                if ($h) {
                    $rows += [pscustomobject]@{
                        Axis='frame time'; Metric=("frames over 33.3 ms ({0} arm)" -f $side[0])
                        Old='--'; New='--'; Result='OMITTED'; Replicates='--'; Caveat=$h
                    }
                }
            }
            if ($arm.Count -gt 0) {
                $identity = Test-MarchIdentity -RasterStats $cs -MarchLeg (@($arm)[0]) -Floor $NoiseFloorMs
            }
        }
    }

    # ---- loading ------------------------------------------------------------
    if ($Axes -contains 'all' -or $Axes -contains 'loading') {
        $ctl = Get-ArmLegs $LegFacts 'load-today'
        $arm = Get-ArmLegs $LegFacts 'load-march'
        $varies = @('voxel.Brick.SuppressQuadMesh')
        $bad = Pair-Check $ctl $arm 'loading' $varies
        if ($bad.Count -gt 0) {
            $refusals += $bad
        } elseif ($ctl.Count -gt 0 -and $arm.Count -gt 0) {
            $cs = Measure-Replicates @($ctl | Where-Object { $null -ne $_.WorkerMsPerChunk } | ForEach-Object { $_.WorkerMsPerChunk })
            $as = Measure-Replicates @($arm | Where-Object { $null -ne $_.WorkerMsPerChunk } | ForEach-Object { $_.WorkerMsPerChunk })
            # The floor here is relative: 5% of the control, since this is a
            # per-chunk cost of order 1 ms and the ms-scale rig floor would be
            # meaninglessly large against it.
            $floor = 0.05
            if ($cs) { $floor = [math]::Max(0.01, 0.05 * $cs.Mean) }
            $cmp = Compare-Arms -ControlStats $cs -ArmStats $as -Floor $floor -Unit 'ms/chunk'
            $rows += [pscustomobject]@{
                Axis='loading'; Metric='worker cost per chunk (total ms / pack count)'
                Old=(Format-Cell $cs.Mean 'N3'); New=(Format-Cell $as.Mean 'N3')
                Result=$cmp.Text
                Replicates=("n={0}/{1}, spread {2:N4}/{3:N4}" -f $cs.N, $as.N, $cs.Spread, $as.Spread)
                Caveat='Worker-thread cost per chunk delivered. NOT chunks/s and NOT wall-clock cold fill. Recomputed as total-over-count; the printed TOTAL line is wrong on the suppression arm and is never read.'
            }
            $cp = Measure-Replicates @($ctl | Where-Object { $null -ne $_.PacksPerSec } | ForEach-Object { [double]$_.PacksPerSec })
            $ap = Measure-Replicates @($arm | Where-Object { $null -ne $_.PacksPerSec } | ForEach-Object { [double]$_.PacksPerSec })
            if ($cp -and $ap) {
                $pfloor = [math]::Max(1.0, 0.05 * $cp.Mean)
                $pcmp = Compare-Arms -ControlStats $cp -ArmStats $ap -Floor $pfloor -Unit 'packs/s'
                $rows += [pscustomobject]@{
                    Axis='loading'; Metric='packs/s (throughput that survives both arms)'
                    Old=(Format-Cell $cp.Mean 'N0'); New=(Format-Cell $ap.Mean 'N0')
                    Result=$pcmp.Text
                    Replicates=("n={0}/{1}" -f $cp.N, $ap.N)
                    Caveat='Measured over the pool''s own first-to-last-pack span. Only meaningful once jobsInFlight is 0 and holding.'
                }
            }
            # Cold fill in seconds: control side only, and the reason prints.
            $cfc = Measure-Replicates @($ctl | Where-Object { $null -ne $_.ColdFillSec } | ForEach-Object { $_.ColdFillSec })
            $armInvalid = Test-MetricValid -Leg (@($arm)[0]) -Metric 'coldFillSec'
            $rows += [pscustomobject]@{
                Axis='loading'; Metric='cold fill to settle (s)'
                Old=(Format-Cell $cfc.Mean 'N1'); New='n/a'
                Result='NOT COMPARED ACROSS THIS PAIR'
                Replicates=("n={0} (old only)" -f $cfc.N)
                Caveat=("{0} Both ends are quantised to the {1}s census interval, so differences under {2}s are not resolvable at all." -f $armInvalid, $Pose.LogInterval, $LoadingFloorSec)
            }
            $lc = Measure-Replicates @($ctl | Where-Object { $null -ne $_.PeakLoaded } | ForEach-Object { [double]$_.PeakLoaded })
            $li = Test-MetricValid -Leg (@($arm)[0]) -Metric 'loadedChunks'
            $rows += [pscustomobject]@{
                Axis='loading'; Metric='chunks published at settle'
                Old=(Format-Cell $lc.Mean 'N0'); New='n/a'
                Result='NOT COMPARED ACROSS THIS PAIR'; Replicates=("n={0} (old only)" -f $lc.N)
                Caveat=$li
            }
        }
    }

    # ---- vram ---------------------------------------------------------------
    if ($Axes -contains 'all' -or $Axes -contains 'vram') {
        $legs = Get-ArmLegs $LegFacts 'vram-both'
        if ($legs.Count -gt 0) {
            $q = Measure-Replicates @($legs | Where-Object { $null -ne $_.QuadContentMiB } | ForEach-Object { $_.QuadContentMiB })
            $b = Measure-Replicates @($legs | Where-Object { $null -ne $_.BrickResidentMiB } | ForEach-Object { $_.BrickResidentMiB })
            $bcav = Test-MetricValid -Leg (@($legs)[0]) -Metric 'brickResidentMiB'
            if ($q -and $b) {
                $ratio = [math]::Round($q.Mean / $b.Mean, 3)
                $rows += [pscustomobject]@{
                    Axis='VRAM at settle'; Metric='CONTENT: resident geometry (MiB)'
                    Old=(Format-Cell $q.Mean 'N1'); New=(Format-Cell $b.Mean 'N1')
                    Result=("brick content is {0}x smaller on the SAME GROUND in the SAME LEG" -f $ratio)
                    Replicates=("n={0}/{1}, spread {2:N1}/{3:N1} MiB" -f $q.N, $b.N, $q.Spread, $b.Spread)
                    Caveat=("Quad content = residentQuads x 8 B at settle. {0} THIS ROW MUST NOT BE CROSSED WITH THE COMMIT ROW BELOW -- doing so is what produced a discredited '3-14x' claim, most of which was allocator slack." -f $bcav)
                }
            }
            $qc = Measure-Replicates @($legs | Where-Object { $null -ne $_.QuadPoolCapacityMB } | ForEach-Object { $_.QuadPoolCapacityMB })
            $bc = Measure-Replicates @($legs | Where-Object { $null -ne $_.BrickCommittedMiB } | ForEach-Object { $_.BrickCommittedMiB })
            if ($qc -and $bc) {
                $rows += [pscustomobject]@{
                    Axis='VRAM at settle'; Metric='COMMIT: pool reservation (MB / MiB)'
                    Old=(Format-Cell $qc.Mean 'N0'); New=(Format-Cell $bc.Mean 'N1')
                    Result='reported side by side; NOT a saving'
                    Replicates=("n={0}/{1}" -f $qc.N, $bc.N)
                    Caveat='These are reservations chosen by their authors, not measurements of what the world needs. A commit ratio is a sizing decision, not a representation result. Compare content with content.'
                }
            }
            $cp = Measure-Replicates @($legs | Where-Object { $null -ne $_.QuadPoolCapacityPct } | ForEach-Object { $_.QuadPoolCapacityPct })
            if ($cp) {
                $rows += [pscustomobject]@{
                    Axis='VRAM at settle'; Metric='quad pool saturation at this pose (%)'
                    Old=(Format-Cell $cp.Mean 'N1'); New='--'; Result='context for the commit row'
                    Replicates=("n={0}" -f $cp.N)
                    Caveat='This pose is not the saturating one. The shipping default refused 34,937 chunks at the temperate pose -- that is a different leg and is not measured here.'
                }
            }
        }
    }

    # ---- geometry -----------------------------------------------------------
    if ($Axes -contains 'all' -or $Axes -contains 'geometry') {
        $ctl = Get-ArmLegs $LegFacts 'geom-raster'
        $arm = Get-ArmLegs $LegFacts 'geom-march'
        $varies = @('voxel.March','voxel.March.Source','voxel.Stream.GPUCullDebugDrawNothing')
        $bad = Pair-Check $ctl $arm 'geometry submitted' $varies
        if ($bad.Count -gt 0) {
            $refusals += $bad
        } elseif ($ctl.Count -gt 0 -and $arm.Count -gt 0) {
            $cq = Measure-Replicates @($ctl | Where-Object { $null -ne $_.TotalQuadsPerGather } | ForEach-Object { [double]$_.TotalQuadsPerGather })
            $aq = Measure-Replicates @($arm | Where-Object { $null -ne $_.TotalQuadsPerGather } | ForEach-Object { [double]$_.TotalQuadsPerGather })
            $cav = Test-MetricValid -Leg (@($arm)[0]) -Metric 'quadsPerGather'
            $result = 'measured'
            if ($aq -and $aq.Mean -eq 0 -and -not $cav) {
                $result = 'MEASURED ZERO -- gathers still counted, quads went to zero'
            }
            $rows += [pscustomobject]@{
                Axis='geometry submitted'; Metric='quads per camera gather (camera + shadow)'
                Old=(Format-Cell $cq.Mean 'N0'); New=(Format-Cell $aq.Mean 'N0')
                Result=$result
                Replicates=("n={0}/{1}" -f $cq.N, $aq.N)
                Caveat=("{0} Cumulative quads over cumulative gathers, differenced across the settled window only. The 'new' arm EMULATES Phase 4 by suppressing submission; the quad pool is still resident and still costs its VRAM." -f $cav)
            }
            $s = Measure-Replicates @($ctl | Where-Object { $null -ne $_.ShadowMultiplier } | ForEach-Object { $_.ShadowMultiplier })
            if ($s) {
                $rows += [pscustomobject]@{
                    Axis='geometry submitted'; Metric='shadow multiplier S on the raster path'
                    Old=(Format-Cell $s.Mean 'N3'); New='--'
                    Result='geometry the raster path resubmits for shadows'
                    Replicates=("n={0}" -f $s.N)
                    Caveat='S = total/camera quads per camera gather. A marcher pays one secondary ray per primary hit against a pyramid already resident and already being traversed -- but that is a design property, not a measurement, until a shadowed marcher arm runs.'
                }
            }
        }
    }

    return [pscustomobject]@{ Rows=$rows; Refusals=$refusals; Identity=$identity }
}

# ==============================================================================
# SECTION 14 -- SELF-TEST
# ==============================================================================
#
# THE INSTRUMENT IS PROVED AGAINST LEGS WHOSE ANSWERS ARE ALREADY WRITTEN DOWN,
# before it is trusted with a number nobody knows.
#
# The positive cases check that the parser reproduces figures already recorded in
# docs/measurements/armA-drawpath-ceiling-2026-08-19.txt. The NEGATIVE cases
# matter more: they check that the driver REFUSES things a naive parser would
# happily report. A parser that gets the easy cases right and the refusals wrong
# is precisely the instrument this project keeps building by accident.

$SelfTestCases = @(
    # ---- positives ----------------------------------------------------------
    @{ Kind='cvar'; Leg='p0-sh-quality-ctl'; Cvar='r.ShadowQuality'; Expect='0'
       Why='THE HEADLINE NEGATIVE-TURNED-POSITIVE. The last LogConfig Set CVar in this log is 5, at line 941. The leg ran 0, set by -ExecCmds at line 1807 in a format with no log category. A parser that stops at "Set CVar" inverts the shadow result.' }
    @{ Kind='cvar'; Leg='p0-sh-quality-r1'; Cvar='r.ShadowQuality'; Expect='5'
       Why='the fixed arm: the [ConsoleVariables] ini block lands after both scalability passes and outranks them.' }
    @{ Kind='frame'; Leg='p0-sh-quality-ctl'; Field='FrameP50'; Expect=18.85; Tol=0.001
       Why='recorded p50 for the shadowless control.' }
    @{ Kind='frame'; Leg='p0-sh-nooctree-r1'; Field='FrameP50'; Expect=34.72; Tol=0.001
       Why='recorded p50 with shadows live.' }
    @{ Kind='frame'; Leg='p3-ident-mode0-r1'; Field='FrameP50'; Expect=19.72; Tol=0.001
       Why='the identity check control.' }
    @{ Kind='frame'; Leg='p3-ident-mode2-r1'; Field='FrameP50'; Expect=25.23; Tol=0.001
       Why='the identity check marcher arm.' }
    @{ Kind='march'; Leg='p3-ident-mode2-r1'; Field='MarchMs'; Expect=5.206; Tol=0.001
       Why='one of six marchMs measurements inside a 3% spread.' }
    @{ Kind='worker'; Leg='p5-phase5-r1'; Expect=0.389; Tol=0.002
       Why='R4 IN ACTION. The log PRINTS 1.127 ms/chunk by summing three per-chunk means over denominators differing by 870x. total_ms/pack_count is 0.389 and reverses the verdict. The parser must never read the printed total.' }
    @{ Kind='worker'; Leg='p5-today-r1'; Expect=0.969; Tol=0.002
       Why='the matched control for the 2.49x Phase 5 result.' }
    @{ Kind='brick'; Leg='p5-phase5-r1'; Field='BrickChunks'; Expect=87643
       Why='coverage held on the suppression arm.' }

    # ---- negatives: the driver must REFUSE these ----------------------------
    @{ Kind='metric-invalid'; Leg='p2-suppressmesh-r1'; Metric='loadedChunks'
       Why='`loaded=` peaked at 3,243 here against 50,504 elsewhere because quad meshing was suppressed and that counter tracks geometry publication. The pool still reached 87,753. The metric must be refused on this arm, not reported.' }
    @{ Kind='metric-invalid'; Leg='p0-sh-nooctree-r1'; Metric='hitches'
       Why='4,740 hitches of 6,742 frames is 70%, against a p50 of 34.72 and a p95 of 35.38 -- a 0.66 ms spread cannot have 70% outliers. The fixed 33.3 ms threshold makes this the median restated. It must be labelled DEGENERATE.' }
    @{ Kind='fingerprint-differ'; LegA='p0-sh-quality-ctl'; LegB='p0-sh-nooctree-r1'; Key='r.ShadowQuality'
       Why='THE R2 CASE. 18.85 ms and 34.72 ms are both real and describe different games. The driver must refuse to put them in one table and must NAME r.ShadowQuality as the reason.' }
    @{ Kind='metric-invalid'; Leg='p3b1-src1-r1'; Metric='marchMs'; Optional=$true
       Why='the first brick-source A/B drew 1,138 of 21,340 tiles at 20.6% MORE cost. A miss runs to the full step budget, so that is wasted stepping, not an indirection price. Must not be reported as a cost result.' }

    # ---- end to end: the row builder and the identity check -----------------
    # Everything above tests one parser in isolation. This tests the whole path
    # -- fingerprint compare, replicate stats, noise bar, row text and the
    # identity -- against the pair that closed the identity check on record.
    @{ Kind='identity'; LegA='p3-ident-mode0-r1'; LegB='p3-ident-mode2-r1'
       ExpectResidual=0.137; Tol=0.002
       Why='THE END-TO-END CASE. 25.23 - 19.72 = 5.51 measured against 5.206 + 0.080 + 0.087 = 5.373 bracketed, residual 0.137 ms inside the 0.18 ms floor. If the row builder mis-assembles any of the five inputs this number moves, and it is the check that says nothing is being paid outside the brackets.' }
    @{ Kind='rows'; LegA='p3-ident-mode0-r1'; LegB='p3-ident-mode2-r1'
       ExpectDelta=5.51; Tol=0.002
       Why='the frame-time row itself: the driver must resolve a 5.51 ms delta and report the marcher as the SLOWER arm, which is the true state today and the one a harness built to flatter the project would quietly round away.' }
)

function Invoke-SelfTest {
    Write-Host ''
    Write-Host '=== SELF-TEST: the instrument, against legs whose answers are already written down ===' -ForegroundColor Cyan
    Write-Host 'Launches nothing. Positives check the parser reproduces recorded figures;'
    Write-Host 'NEGATIVES check the driver refuses what a naive parser would happily report.'
    Write-Host ''

    $cache = @{}
    function Fetch { param($n)
        if (-not $cache.ContainsKey($n)) { $cache[$n] = Get-LegFacts -Name $n -Arm $null }
        return $cache[$n]
    }

    $pass = 0; $fail = 0; $skip = 0
    foreach ($c in $SelfTestCases) {
        $name = if ($c.LegA) { "$($c.LegA) vs $($c.LegB)" } else { $c.Leg }
        $label = "{0,-22} {1,-18}" -f $c.Kind, $name

        $legPath = $null
        if ($c.Leg) { $legPath = Join-Path $SavedDir "$($c.Leg).log" }
        if ($legPath -and -not (Test-Path $legPath)) {
            if ($c.Optional) { Write-Host "  SKIP $label -- log not present (optional case)" -ForegroundColor DarkGray; $skip++; continue }
            Write-Host "  FAIL $label -- log not present" -ForegroundColor Red; $fail++; continue
        }

        $ok = $false; $got = ''
        switch ($c.Kind) {
            'cvar' {
                $f = Fetch $c.Leg
                $v = $f.Cvars[$c.Cvar]
                if (-not $v) { $v = Get-CvarFinal -Resolved (Resolve-CvarHistory (Get-Content $legPath)) -Name $c.Cvar }
                $got = "$($v.Value) (set by $($v.Setter), line $($v.Line))"
                $ok = ($v.Value -eq $c.Expect)
            }
            'frame' {
                $f = Fetch $c.Leg
                $val = $f.PSObject.Properties[$c.Field].Value
                $got = "$val"
                $ok = ($null -ne $val -and [math]::Abs($val - $c.Expect) -le $c.Tol)
            }
            'march' {
                $f = Fetch $c.Leg
                $val = $f.PSObject.Properties[$c.Field].Value
                $got = "$val"
                $ok = ($null -ne $val -and [math]::Abs($val - $c.Expect) -le $c.Tol)
            }
            'worker' {
                $f = Fetch $c.Leg
                $got = "$($f.WorkerMsPerChunk) (mesh $($f.MeshMs)/$($f.MeshCount), fill $($f.FillMs)/$($f.FillCount), pack $($f.PackMs)/$($f.PackCount))"
                $ok = ($null -ne $f.WorkerMsPerChunk -and [math]::Abs($f.WorkerMsPerChunk - $c.Expect) -le $c.Tol)
            }
            'brick' {
                $f = Fetch $c.Leg
                $val = $f.PSObject.Properties[$c.Field].Value
                $got = "$val"
                $ok = ($val -eq $c.Expect)
            }
            'metric-invalid' {
                $f = Fetch $c.Leg
                $r = Test-MetricValid -Leg $f -Metric $c.Metric
                $got = if ($r) { $r } else { '<reported as valid>' }
                $ok = ($null -ne $r)
            }
            'fingerprint-differ' {
                $a = Fetch $c.LegA; $b = Fetch $c.LegB
                $d = Compare-Fingerprints -LegA $a -LegB $b -AllowedToVary @()
                $keys = @($d | ForEach-Object { $_.Key })
                $got = "differs in: $($keys -join ', ')"
                $ok = ($keys -contains $c.Key)
            }
            'identity' {
                $a = Fetch $c.LegA; $b = Fetch $c.LegB
                $cs = Measure-Replicates @($a.FrameP50)
                $id = Test-MarchIdentity -RasterStats $cs -MarchLeg $b -Floor $NoiseFloorMs
                $got = $id.Text
                $ok = ($null -ne $id.Residual -and [math]::Abs($id.Residual - $c.ExpectResidual) -le $c.Tol -and $id.Closed -eq $true)
            }
            'rows' {
                $a = Fetch $c.LegA; $b = Fetch $c.LegB
                $cs = Measure-Replicates @($a.FrameP50)
                $as = Measure-Replicates @($b.FrameP50)
                $cmp = Compare-Arms -ControlStats $cs -ArmStats $as -Floor $NoiseFloorMs -Unit 'ms'
                $got = $cmp.Text
                $ok = ($cmp.Verdict -eq 'RESOLVED' -and [math]::Abs($cmp.Delta - $c.ExpectDelta) -le $c.Tol)
            }
        }

        if ($ok) { Write-Host "  PASS $label  $got" -ForegroundColor Green; $pass++ }
        else {
            Write-Host "  FAIL $label" -ForegroundColor Red
            Write-Host "       expected: $($c.Expect)$($c.Key)$($c.Metric)" -ForegroundColor Red
            Write-Host "       got:      $got" -ForegroundColor Red
            Write-Host "       why it matters: $($c.Why)" -ForegroundColor DarkYellow
            $fail++
        }
    }

    Write-Host ''
    Write-Host ("SELF-TEST: {0} pass, {1} fail, {2} skipped" -f $pass, $fail, $skip) -ForegroundColor $(if ($fail -eq 0) { 'Green' } else { 'Red' })
    if ($fail -gt 0) {
        Write-Host 'DO NOT RUN THE FINAL COMPARISON WITH A FAILING SELF-TEST. A parser that mis-reads a known leg will mis-read an unknown one silently.' -ForegroundColor Red
    }
    return ($fail -eq 0)
}

# ==============================================================================
# SECTION 15 -- MAIN
# ==============================================================================

if ($SelfTest) {
    $ok = Invoke-SelfTest
    if ($ok) { exit 0 } else { exit 1 }
}

$build = Get-BuildFingerprint
$shaderCheck = Test-ShaderTreeAgreesWithBinary -Build $build

if ($VerifyOnly) {
    if (-not $Legs) { throw "-VerifyOnly needs -Legs 'logname=armId' pairs." }
    $facts = @()
    foreach ($spec in $Legs) {
        $kv = $spec -split '=', 2
        $arm = $ArmCatalogue | Where-Object { $_.Id -eq $kv[1] } | Select-Object -First 1
        if (-not $arm -and $kv.Count -gt 1) { Write-Host "  unknown armId '$($kv[1])' -- parsed without an arm, so R1 cannot be applied" -ForegroundColor Yellow }
        $facts += Get-LegFacts -Name $kv[0] -Arm $arm
    }
    $built = Build-Rows -LegFacts $facts -Axes $Axis
    $out = Join-Path $OutDir "final-comparison-$Tag.md"
    New-Report -LegFacts $facts -Rows $built.Rows -Refusals $built.Refusals -Identity $built.Identity -Build $build -Path $out
    exit 0
}

$plan = Get-LegPlan -Axes $Axis -Reps $Replicates

if (-not $Execute) {
    Write-Plan -Plan $plan -Build $build -ShaderCheck $shaderCheck
    exit 0
}

if (-not $shaderCheck.Agrees) { throw $shaderCheck.Text }

# THE SELF-TEST IS A PRECONDITION OF SPENDING LEGS, not a separate chore. A
# parser that mis-reads a leg whose answer is written down will mis-read one
# whose answer is not, and will do it silently.
if (-not (Invoke-SelfTest)) { throw 'Self-test failed. Refusing to spend legs on an instrument that mis-reads a known log.' }

$done = Invoke-Plan -Plan $plan -Build $build

$facts = @()
foreach ($e in $done) { $facts += Get-LegFacts -Name $e.LegName -Arm $e.Arm }

# The overlap audit is a separate, after-the-fact proof that no two legs shared
# the box: the guard at launch checks liveness at START and cannot speak for a
# run that was already finishing. A contended leg reads as a slow configuration
# and nothing in the log says otherwise.
#
# ITS RESULT IS NOT ADVISORY. If it reports contamination the report is still
# written -- the legs are already spent and the parse is still worth having --
# but every leg is marked void first, because contention is MUTUAL and voiding
# only the later leg of an overlap is the mistake this audit exists to prevent.
& (Join-Path $PSScriptRoot 'voxel-audit-leg-overlap.ps1') -LogName @($done | ForEach-Object { $_.LegName })
$overlapCode = $LASTEXITCODE
if ($overlapCode -ne 0) {
    Write-Host 'LEG OVERLAP DETECTED. Voiding EVERY leg in this set, not just the later side of each overlap.' -ForegroundColor Red
    foreach ($f in $facts) {
        $f.Valid = $false
        $f.VoidReasons += 'LEG OVERLAP: this set shared the box. Contention is mutual, so both sides of every overlap are void, and a contended leg reads as a slow configuration with nothing in its own log to say so.'
    }
}

$built = Build-Rows -LegFacts $facts -Axes $Axis
$out = Join-Path $OutDir "final-comparison-$Tag.md"
New-Report -LegFacts $facts -Rows $built.Rows -Refusals $built.Refusals -Identity $built.Identity -Build $build -Path $out
