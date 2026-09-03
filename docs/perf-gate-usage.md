# The standing perf gate: two commands

Written 2026-09-02. Covers `tools/voxel-perf-gate.ps1` and the checker it wraps,
`tools/check-perf-run.py`.

## The two commands

**1. Run the leg.** This is the only step that launches the editor. One at a
time — a second editor on the box corrupts both measurements.

    pwsh tools/voxel-run-flight-leg.ps1 -LogName my-leg -RunSec 120 -Width 2560 -Height 1440

It writes two artifacts:

| artifact | path |
| --- | --- |
| perf summary | `ue-project/Saved/PerfRuns/perf_<stamp>.json` (written at the END of the leg) |
| capture log | `Saved/my-leg.log` (written THROUGHOUT the leg) |

**2. Gate it.** This step launches nothing. Give it both artifacts:

    pwsh tools/voxel-perf-gate.ps1 -PerfJson ue-project\Saved\PerfRuns\perf_<stamp>.json -CaptureLog Saved\my-leg.log

With no arguments it takes the newest of each and gates the pairing itself (see
below). To gate the perf JSON alone, pass `-NoCaptureLog`.

## Exit codes

Both scripts use the same three, so a caller can gate on either.

| code | meaning | what to do |
| --- | --- | --- |
| **0** | every check ran and passed | nothing |
| **1** | at least one check FAILED | read the table; this is a real result |
| **2** | the run could not be gated at all | fix the inputs and run it again — **this is never a pass** |

Exit 2 is the one worth understanding. It covers: a missing or unreadable
artifact; a log that is still being written; **no thresholds supplied**; a perf
JSON whose frame times are marked inadmissible. Each of those used to be able
to look like success, and every one of them means nothing was tested.

## What the gate checks

| check | source | fails when |
| --- | --- | --- |
| perf thresholds | perf JSON, via `check-perf-run.py` | p95 or post-warmup p95 over the ceiling (default 33.3 ms) |
| artifact pairing | file timestamps | the JSON and the log are more than `-MaxPairGapMinutes` (default 120) apart, i.e. they are from different legs |
| fine tier armed | `Fine tier ENABLED` in the log | the line is absent — the leg ran on the coarse tier and its terrain numbers are about a different world |
| fine tiles loaded | `Fine tier (window): ... loaded=N` | `N` below `-MinFineTilesLoaded` (default 1) — an armed tier that loaded nothing |
| fine tier gate | same line, `gateLeaks=` | nonzero — a query was answered with sea level, so this run's terrain is not reproducible |
| hole census | last `holes breakdown` line | it carries a real `SHORTFALL` marker |
| conservation | last `cover funnel` line | a real `VIOLATED` line appears anywhere in the log |

A check with nothing to read reports **SKIP**, never PASS. A table with no PASS
row in it exits 2.

## Thresholds: which bar you are gating against

The default is **p95 ≤ 33.3 ms**. That is the M1 gate from
`docs/debug-tooling-plan.md`, and it is the engine's own `hitchThresholdMs`, so
the whole archive is comparable against it. It is the legacy 30-fps bar.

**It is not the live product goal.** `docs/SCOREBOARD.md` Goal 3 is ">100 FPS
while MOVING at ≥ 20 m/s", which is **p95 < 10 ms**. Gate against that with:

    pwsh tools/voxel-perf-gate.ps1 -MaxP95Ms 10 -MaxPostWarmupP95Ms 10 ...

Passing the default is not the same as meeting Goal 3, and the gate says so on
every pass.

Other overrides: `-MaxHitches`, `-MaxMaxMs`, `-MinAvgChunksPerSec` (all unset by
default, so unset means ungated), `-StaleSeconds` / `-AllowLiveLog`,
`-AllowInadmissibleTiming`, `-MaxPairGapMinutes 0` to skip the pairing check.

## Three traps the gate handles, and you should too if you grep by hand

1. **`grep VIOLATED` goes off on every passing leg.** The conservation law's
   SUCCESS text is: `cover funnel CONSERVED -- offered 644 == admitted 644 ...
   this line MUST read VIOLATED there, or the law is decorative.` The word is in
   the passing line, as instructions. Same in `VoxelGI.cpp`: the HELD line ends
   `any later divergence logs GI ARM VIOLATED.` Match the emit sites' real
   wording (`funnel VIOLATED`, `ARM VIOLATED:`), not the bare word.

2. **`grep SHORTFALL` goes off on `SHORTFALL=0`.** `VoxelFloodTest` prints that
   as a routine counter. Only a nonzero value, or one of the markers
   `[SHORTFALL:`, `MOBILIZATION SHORTFALL`, `INSTRUMENT SHORTFALL`, is a defect.
   As of 2026-09-02, real marcher legs carry `[SHORTFALL: capture missed rays]`
   on some EARLY census windows and a clean last one — the gate reads the last
   line and reports the earlier count as a note, not a failure.

3. **`loaded=` is on three different lines.** The streaming line's is chunks
   this window, the rings line's is chunks per ring, and only the
   `Fine tier (window)` line's is the TILE count. That one is
   `TilesLoadedSinceStart()` — cumulative and monotonic — so the last such line
   is the run total. `unloaded=` also contains the substring; anchor on `\b`.

## What is in CI, and what is not

`.github/workflows/ci.yml` job **`tools-python`** runs, on every push and PR:

    python -m py_compile tools/*.py
    python tools/check-perf-run.py --selftest

`--selftest` fabricates perf summaries in a temp directory and asserts the
exit code for each of 11 cases — including that supplying no thresholds exits 2.
It was verified to go red when the old vacuous pass is reintroduced.

**No perf leg runs in CI and none can.** A `-VoxelPerfRun` leg needs the 30 GB
engine install, a GPU, and twenty minutes; a hosted runner has a 14 GB disk (the
same measurement that keeps the UE module out of CI — see
`.github/workflows/ue-build.yml`). CI checks the gate's code. A human or a
self-hosted box runs the gate.
