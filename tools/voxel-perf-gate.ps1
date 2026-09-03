# voxel-perf-gate.ps1 -- the standing gate you run AFTER a leg has finished.
#
# WHAT IT IS. One command that turns "the leg finished" into a PASS/FAIL table
# with a nonzero exit code. It reads two artifacts a finished leg leaves behind:
#
#   ue-project/Saved/PerfRuns/perf_<stamp>.json   the -VoxelPerfRun summary
#   Saved/capture-<name>.log                      the leg's log (optional)
#
# WHAT IT DELIBERATELY DOES NOT DO: launch anything. It never starts the editor,
# never runs a leg, never writes into Saved/. If an artifact it needs is not
# there it says so and exits 2. That is not politeness, it is the box rule --
# two editors on one machine corrupt each other's measurements, and a gate that
# helpfully "just runs the leg for you" is a second editor. Run the leg first
# (tools/voxel-run-flight-leg.ps1), then run this.
#
# EXIT CODES, mirroring tools/check-perf-run.py so a caller can gate on either:
#   0  every check ran and passed
#   1  at least one check FAILED
#   2  the gate could not be run at all -- a missing artifact, a log still being
#      written, a perf JSON whose numbers are inadmissible. NEVER a pass.
#
# ---------------------------------------------------------------------------
# THREE THINGS THIS SCRIPT KNOWS THAT A NAIVE VERSION WOULD GET WRONG.
#
# 1. `grep VIOLATED` FALSE-POSITIVES ON EVERY PASSING LEG. The conservation law
#    prints, on success:
#        "cover funnel CONSERVED -- offered 644 == admitted 644 ... this line
#         MUST read VIOLATED there, or the law is decorative."
#    The word VIOLATED is in the PASSING text, as instructions to the reader.
#    Same shape in VoxelGI.cpp: the HELD line ends "any later divergence logs
#    GI ARM VIOLATED." So this script matches the emit sites' actual violation
#    wording (`funnel VIOLATED`, `ARM VIOLATED:`, `GATE VIOLATED`) and
#    explicitly discounts the prose forms. A gate that cried wolf on every green
#    leg would be switched off inside a week, which is the real failure.
#
# 2. `grep SHORTFALL` FALSE-POSITIVES ON `SHORTFALL=0`. VoxelFloodTest prints
#    "debited=.. credited=.. SHORTFALL=0" as a routine counter. Only a NONZERO
#    value is a defect, alongside the bracketed markers `[SHORTFALL:`,
#    `MOBILIZATION SHORTFALL` and `INSTRUMENT SHORTFALL`.
#
# 3. `loaded=` APPEARS ON THREE DIFFERENT LINES. The streaming line's `loaded=`
#    is chunks this window, the rings line's is chunks per ring, and only the
#    "Fine tier (window)" line's is the TILE count -- and that one is
#    TilesLoadedSinceStart(), i.e. cumulative and monotonic, so the LAST such
#    line in the log is the run total. This script reads that line and no other.
#    (`\bloaded=` also keeps `unloaded=` out of the match.)
#
# THE THRESHOLD DEFAULTS. -MaxP95Ms 33.3 is the M1 gate (docs/debug-tooling-
# plan.md; 33.3 ms is also the engine's own hitchThresholdMs). It is the
# LEGACY 30-fps bar and it is deliberately the default here, because it is the
# bar the whole archive is comparable against. The live product goal is much
# harder -- docs/SCOREBOARD.md Goal 3, ">100 FPS while MOVING", is p95 < 10 ms
# -- so gate against that with `-MaxP95Ms 10`. Passing this script's default is
# not the same as meeting Goal 3, and the table says so.

[CmdletBinding()]
param(
    # Default: newest perf_*.json in ue-project/Saved/PerfRuns.
    [string]$PerfJson,

    # Default: newest Saved/capture-*.log. Optional -- with -NoCaptureLog the
    # log half of the gate is skipped and the table says SKIP, never PASS.
    [string]$CaptureLog,
    [switch]$NoCaptureLog,

    # --- perf thresholds, all overridable ---------------------------------
    [double]$MaxP95Ms = 33.3,
    [double]$MaxPostWarmupP95Ms = 33.3,
    [Nullable[int]]$MaxHitches,               # unset = not gated (M2, not M1)
    [Nullable[double]]$MaxMaxMs,
    [Nullable[double]]$MinAvgChunksPerSec,
    [switch]$AllowInadmissibleTiming,

    # --- log thresholds ----------------------------------------------------
    [long]$MinFineTilesLoaded = 1,

    # The JSON is written at the end of the leg; the log is written throughout
    # it. So they should be minutes apart, and a large gap means the two
    # artifacts are from DIFFERENT legs -- which is a gate reporting PASS on a
    # pairing nobody checked. 0 disables the check.
    [double]$MaxPairGapMinutes = 120,

    # A leg that finished seconds ago may still be flushing. voxel-leg-summary
    # exists because reading a half-written log produced three false
    # regressions in one session; this is the same guard, cheaper.
    [int]$StaleSeconds = 45,
    [switch]$AllowLiveLog,

    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..").Path

# ---------------------------------------------------------------------------
# Result table. Every check appends exactly one row. A row is PASS, FAIL, SKIP
# or UNUSABLE -- there is no way to reach the end with no rows and exit 0.
# ---------------------------------------------------------------------------
$script:Rows = @()
function Add-Row {
    param([string]$Check, [string]$Result, [string]$Detail)
    $script:Rows += [pscustomobject]@{ Check = $Check; Result = $Result; Detail = $Detail }
}

function Stop-Unusable {
    param([string]$Message)
    Write-Host ''
    Write-Host "GATE UNUSABLE (exit 2): $Message" -ForegroundColor Red
    Write-Host 'Nothing was tested. This is a configuration error, not a pass.' -ForegroundColor Red
    exit 2
}

# ---------------------------------------------------------------------------
# RESOLVE INPUTS. Never launch; assert.
# ---------------------------------------------------------------------------
$PerfDir = Join-Path $Root 'ue-project\Saved\PerfRuns'
if (-not $PerfJson) {
    if (-not (Test-Path $PerfDir)) {
        Stop-Unusable "no $PerfDir -- no -VoxelPerfRun leg has ever finished on this box. Run a leg first (tools/voxel-run-flight-leg.ps1). This script does not launch the editor."
    }
    $newest = Get-ChildItem -Path $PerfDir -Filter 'perf_*.json' -File |
              Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $newest) {
        Stop-Unusable "no perf_*.json in $PerfDir -- run a leg with -VoxelPerfRun=<seconds> first. This script does not launch the editor."
    }
    $PerfJson = $newest.FullName
    Write-Host "PerfJson (newest): $PerfJson" -ForegroundColor DarkGray
}
if (-not (Test-Path $PerfJson)) {
    Stop-Unusable "-PerfJson '$PerfJson' does not exist."
}
$PerfItem = Get-Item $PerfJson

$SavedDir = Join-Path $Root 'Saved'
if ($NoCaptureLog) {
    $CaptureLog = $null
} elseif (-not $CaptureLog) {
    if (Test-Path $SavedDir) {
        $newestLog = Get-ChildItem -Path $SavedDir -Filter 'capture-*.log' -File |
                     Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($newestLog) { $CaptureLog = $newestLog.FullName }
    }
    if (-not $CaptureLog) {
        Stop-Unusable "no capture-*.log in $SavedDir. Pass -CaptureLog <path>, or -NoCaptureLog to gate the perf JSON alone. This script does not launch the editor."
    }
    Write-Host "CaptureLog (newest): $CaptureLog" -ForegroundColor DarkGray
} elseif (-not (Test-Path $CaptureLog)) {
    Stop-Unusable "-CaptureLog '$CaptureLog' does not exist."
}

# THE PAIRING IS NOT VERIFIED, AND THAT IS ITS OWN CHECK. Nothing in either
# artifact names the other, so "newest JSON + newest log" is a guess -- and the
# first smoke run of this script proved why it needs gating rather than
# printing: it returned GATE PASS on a perf JSON and a capture log written 66
# HOURS apart, i.e. it gated two different legs and called it a result. The
# JSON lands at the end of the leg the log was being written through, so a
# correct pairing is minutes apart.
$gapMin = $null
if ($CaptureLog) {
    $LogItem = Get-Item $CaptureLog
    $gapMin = [math]::Abs([math]::Round(($PerfItem.LastWriteTime - $LogItem.LastWriteTime).TotalMinutes, 1))
    Write-Host ("Artifacts: JSON written {0}, log written {1} (gap {2} min)." -f $PerfItem.LastWriteTime, $LogItem.LastWriteTime, $gapMin) -ForegroundColor DarkGray
}

# Freshness: a file still being written is a running leg, not a finished one.
if (-not $AllowLiveLog) {
    $now = Get-Date
    foreach ($f in @($PerfItem, $(if ($CaptureLog) { Get-Item $CaptureLog } else { $null }))) {
        if ($null -eq $f) { continue }
        $ageSec = ($now - $f.LastWriteTime).TotalSeconds
        if ($ageSec -lt $StaleSeconds) {
            Stop-Unusable ("{0} was written {1:N0} s ago (< -StaleSeconds {2}). A leg that is still running is not a finished leg; a partial log has a plausible everything. Wait, or pass -AllowLiveLog." -f $f.Name, $ageSec, $StaleSeconds)
        }
    }
}

# ---------------------------------------------------------------------------
# CHECK 1: the perf JSON, via tools/check-perf-run.py.
# ---------------------------------------------------------------------------
$Checker = Join-Path $PSScriptRoot 'check-perf-run.py'
if (-not (Test-Path $Checker)) { Stop-Unusable "missing $Checker" }
if (-not (Get-Command $Python -ErrorAction SilentlyContinue)) {
    Stop-Unusable "'$Python' is not on PATH. Pass -Python <path to python.exe>."
}

$checkerArgs = @($Checker, $PerfJson,
                 '--max-p95-ms', $MaxP95Ms.ToString([cultureinfo]::InvariantCulture),
                 '--max-post-warmup-p95-ms', $MaxPostWarmupP95Ms.ToString([cultureinfo]::InvariantCulture))
if ($null -ne $MaxHitches)          { $checkerArgs += @('--max-hitches', $MaxHitches.ToString()) }
if ($null -ne $MaxMaxMs)            { $checkerArgs += @('--max-max-ms', ([double]$MaxMaxMs).ToString([cultureinfo]::InvariantCulture)) }
if ($null -ne $MinAvgChunksPerSec)  { $checkerArgs += @('--min-avg-chunks-per-sec', ([double]$MinAvgChunksPerSec).ToString([cultureinfo]::InvariantCulture)) }
if ($AllowInadmissibleTiming)       { $checkerArgs += '--allow-inadmissible-timing' }

Write-Host ''
Write-Host "--- check-perf-run.py ------------------------------------------" -ForegroundColor Cyan
& $Python @checkerArgs | Where-Object { $_ -notmatch '^\s*[""{}]' } | Write-Host
$checkerExit = $LASTEXITCODE
Write-Host "----------------------------------------------------------------" -ForegroundColor Cyan

if ($checkerExit -eq 0) {
    Add-Row 'perf thresholds' 'PASS' ("p95 <= $MaxP95Ms ms, postWarmup p95 <= $MaxPostWarmupP95Ms ms")
} elseif ($checkerExit -eq 1) {
    Add-Row 'perf thresholds' 'FAIL' 'check-perf-run.py exit 1 -- a threshold was violated (see above)'
} else {
    Add-Row 'perf thresholds' 'UNUSABLE' "check-perf-run.py exit $checkerExit -- the run could not be gated (see above)"
}

# ---------------------------------------------------------------------------
# CHECK 2..5: the capture log.
# ---------------------------------------------------------------------------
if (-not $CaptureLog) {
    Add-Row 'artifact pairing'  'SKIP' '-NoCaptureLog'
    Add-Row 'fine tier armed'   'SKIP' '-NoCaptureLog'
    Add-Row 'fine tiles loaded' 'SKIP' '-NoCaptureLog'
    Add-Row 'hole census'       'SKIP' '-NoCaptureLog'
    Add-Row 'conservation'      'SKIP' '-NoCaptureLog'
} else {
    $lines = Get-Content -LiteralPath $CaptureLog

    # --- 1b. THE PAIRING ITSELF. See the block where $gapMin is computed. ---
    if ($MaxPairGapMinutes -le 0) {
        Add-Row 'artifact pairing' 'SKIP' '-MaxPairGapMinutes 0 (pairing not gated)'
    } elseif ($gapMin -le $MaxPairGapMinutes) {
        Add-Row 'artifact pairing' 'PASS' "JSON and log written $gapMin min apart (<= $MaxPairGapMinutes)"
    } else {
        Add-Row 'artifact pairing' 'FAIL' "JSON and log written $gapMin min apart (> $MaxPairGapMinutes) -- these are almost certainly DIFFERENT legs; pass matching -PerfJson/-CaptureLog"
    }

    # --- 2. ENGAGEMENT PROOF: the fine tier actually turned on. -------------
    # Without this line the leg ran on the coarse tier and every terrain
    # number in it is about a different world.
    $armed = $lines | Where-Object { $_ -clike '*Fine tier ENABLED*' } | Select-Object -First 1
    if ($armed) {
        $detail = 'Fine tier ENABLED'
        if ($armed -match 'provider=(\S+)') { $detail = "provider=$($Matches[1])" }
        Add-Row 'fine tier armed' 'PASS' $detail
    } else {
        Add-Row 'fine tier armed' 'FAIL' "no 'Fine tier ENABLED' line -- this leg ran WITHOUT the fine tier"
    }

    # --- 3. PROOF OF TRAFFIC: tiles actually loaded. ------------------------
    # `loaded=` on the "Fine tier (window)" line only. Cumulative, so the last
    # line is the run total. An armed tier that loaded nothing is the silent
    # success this project keeps finding.
    $fineWindows = $lines | Where-Object { $_ -match 'Fine tier \([^)]*window\):' }
    $lastFine = $fineWindows | Select-Object -Last 1
    if (-not $lastFine) {
        Add-Row 'fine tiles loaded' 'FAIL' "no 'Fine tier (window)' line -- the residency tick never reported"
    } elseif ($lastFine -match '\bloaded=(\d+)') {
        $tiles = [long]$Matches[1]
        $gateLeaks = -1
        if ($lastFine -match 'gateLeaks=(\d+)') { $gateLeaks = [long]$Matches[1] }
        if ($tiles -ge $MinFineTilesLoaded) {
            Add-Row 'fine tiles loaded' 'PASS' "loaded=$tiles (>= $MinFineTilesLoaded), gateLeaks=$gateLeaks"
        } else {
            Add-Row 'fine tiles loaded' 'FAIL' "loaded=$tiles (< $MinFineTilesLoaded) -- no fine tiles came in on this leg"
        }
        # gateLeaks > 0 means a query into a non-resident tile was answered with
        # SEA LEVEL. That run produced terrain that is not reproducible.
        if ($gateLeaks -gt 0) {
            Add-Row 'fine tier gate' 'FAIL' "gateLeaks=$gateLeaks -- a query was answered with sea level; this run's terrain is NOT reproducible"
        }
    } else {
        Add-Row 'fine tiles loaded' 'FAIL' "'Fine tier (window)' line carries no loaded= field -- log format changed, this check is blind"
    }

    # --- 4. THE LAST HOLE CENSUS: no SHORTFALL. -----------------------------
    # See header note 2: SHORTFALL=0 is a routine counter, not a defect.
    $shortfallReal = '\[SHORTFALL:|MOBILIZATION SHORTFALL|INSTRUMENT SHORTFALL|SHORTFALL=[1-9]'
    $census = $lines | Where-Object { $_ -match 'holes breakdown' } | Select-Object -Last 1
    $shortfallAnywhere = @($lines | Where-Object { $_ -cmatch $shortfallReal }).Count
    if (-not $census) {
        Add-Row 'hole census' 'SKIP' 'no "holes breakdown" line in this log (not a marcher leg)'
    } elseif ($census -cmatch $shortfallReal) {
        Add-Row 'hole census' 'FAIL' "last census line reads SHORTFALL ($shortfallAnywhere marked line(s) in the log)"
    } else {
        $note = 'last census line clean'
        if ($shortfallAnywhere -gt 0) { $note = "last census line clean; $shortfallAnywhere earlier line(s) marked SHORTFALL -- worth a look, not a gate" }
        Add-Row 'hole census' 'PASS' $note
    }

    # --- 5. THE LAST CONSERVATION LINE: not VIOLATED. -----------------------
    # See header note 1: the PASSING text contains the word VIOLATED.
    $violatedReal = 'funnel VIOLATED|ARM VIOLATED:|GATE VIOLATED'
    $violatedProse = 'read VIOLATED|reads VIOLATED|logs GI ARM VIOLATED'
    $conservation = $lines | Where-Object { $_ -match 'cover funnel|ARM VIOLATED|funnel VIOLATED' } | Select-Object -Last 1
    $realViolations = @($lines | Where-Object { ($_ -cmatch $violatedReal) -and ($_ -notmatch $violatedProse) })
    if ($realViolations.Count -gt 0) {
        Add-Row 'conservation' 'FAIL' ("{0} VIOLATED line(s); first: {1}" -f $realViolations.Count, ($realViolations[0] -replace '^\[[^]]*\]\[[^]]*\]', '').Substring(0, [math]::Min(90, ($realViolations[0] -replace '^\[[^]]*\]\[[^]]*\]', '').Length)))
    } elseif ($conservation) {
        Add-Row 'conservation' 'PASS' 'last conservation line reads CONSERVED (the word VIOLATED in it is its own instructions)'
    } else {
        Add-Row 'conservation' 'SKIP' 'no conservation line in this log'
    }
}

# ---------------------------------------------------------------------------
# THE TABLE, AND THE VERDICT.
# ---------------------------------------------------------------------------
Write-Host ''
$script:Rows | Format-Table -AutoSize -Wrap | Out-String | Write-Host

$fails    = @($script:Rows | Where-Object { $_.Result -eq 'FAIL' })
$unusable = @($script:Rows | Where-Object { $_.Result -eq 'UNUSABLE' })
$passes   = @($script:Rows | Where-Object { $_.Result -eq 'PASS' })

# ORDER MATTERS, and the first version of this block got it wrong in a way its
# own demo caught: the "nothing ran" guard was first, so a log where EVERY check
# failed had no PASS row and was reported UNUSABLE (exit 2) instead of FAIL
# (exit 1). A gate that reports its worst input as "could not be tested" is the
# same defect it exists to catch. Decide the verdict first; the nothing-ran
# guard is only for a table with no decidable row in it at all.
if ($unusable.Count -gt 0) {
    Write-Host ("GATE UNUSABLE (exit 2): {0} check(s) could not be run." -f $unusable.Count) -ForegroundColor Red
    exit 2
}
if ($fails.Count -gt 0) {
    Write-Host ("GATE FAIL (exit 1): {0} of {1} check(s) failed." -f $fails.Count, $script:Rows.Count) -ForegroundColor Red
    exit 1
}
if ($passes.Count -eq 0) {
    Write-Host 'GATE UNUSABLE (exit 2): every check was skipped -- not one of them ran. That is not a pass.' -ForegroundColor Red
    exit 2
}
Write-Host ("GATE PASS: {0} check(s) passed, {1} skipped." -f $passes.Count, ($script:Rows.Count - $passes.Count)) -ForegroundColor Green
Write-Host "Note: the p95 default is the 33.3 ms M1 bar. Goal 3 (>100 fps moving) is p95 < 10 ms -- gate that with -MaxP95Ms 10." -ForegroundColor DarkGray
exit 0
