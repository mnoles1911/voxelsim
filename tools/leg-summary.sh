#!/usr/bin/env bash
# Summarise a leg log over its ACTIVE windows only.
#
# THIS EXISTS BECAUSE `grep ... | tail -1` READS THE POST-FLIGHT LINGER WINDOW,
# which is all zeros: the run's own clock keeps logging for LingerSec after the
# flight ends. Reading it produced "the cache is enabled and doing nothing" and
# "dispatched=0" twice in one session, both wrong. A window with no jobs in it
# is not a measurement -- this script drops those windows and aggregates the rest.
for f in "$@"; do
  log="Saved/$f.log"
  [ -f "$log" ] || { echo "$f: MISSING"; continue; }
  printf "%-18s " "$f"
  grep "VoxelPerfRun complete" "$log" | tail -1 | grep -o "frames=[0-9]* p50=[0-9.]*ms p95=[0-9.]*ms.*hitches=[0-9]*" | tr -d '\n'
  packs=$(grep -o 'brickPacks=[0-9]*' "$log" | sort -t= -k2 -n | tail -1 | cut -d= -f2)
  holes=$(grep -o 'holes=[0-9]* scanned=[0-9]*' "$log" | tail -1)
  cyc=$(grep -o "cycPerColumn=[0-9]*" "$log" | awk -F= '{if($2>0){s+=$2;n++}} END{if(n)printf "%.0f",s/n; else printf "n/a"}')
  disp=$(grep -o "job flow ([^)]*window): dispatched=[0-9]*" "$log" | awk -F= '{s+=$NF} END{printf "%d",s}')
  echo " | packs=$packs cycPerColumn=$cyc dispatched=$disp $holes"
  # The GPU fork's submit->deliver STAGE PARTITION, aggregated across active
  # windows only (each window weighted by its complete-job count n, so quiet
  # linger windows contribute nothing). sum= is recomputed HERE from the four
  # stage means rather than trusted from any single window -- if sum and
  # submitToDeliver disagree by more than rounding, a stage went missing and
  # every per-stage conclusion from the leg is void. Only the mean section
  # (before the '|') is parsed; the p50/p95 section reuses the same key names.
  gpustages=$(grep -o "Voxel GPU mesh stages ([^|]*" "$log" | awk '
    function grab(s, key) { if (match(s, key "=[0-9.]+")) { return substr(s, RSTART+length(key)+1, RLENGTH-length(key)-1) + 0 } return 0 }
    { n = grab($0, "n"); if (n <= 0) next;
      N += n; Q += n*grab($0, "queued"); P += n*grab($0, "promoteToDispatch");
      D += n*grab($0, "dispatchToReady"); R += n*grab($0, "readyToDeliver");
      T += n*grab($0, "submitToDeliver") }
    END { if (N > 0) printf "n=%d queued=%.1f promoteToDispatch=%.1f dispatchToReady=%.1f readyToDeliver=%.1f sum=%.1f submitToDeliver=%.1f", N, Q/N, P/N, D/N, R/N, (Q+P+D+R)/N, T/N }')
  if [ -n "$gpustages" ]; then printf "%-18s   gpuStages: %s\n" "" "$gpustages"; fi
  # B.3 asset-resolve accounting (-VoxelAsyncAssetResolve). Unlike the windowed
  # lines above, these counters are CUMULATIVE since process start, so the LAST
  # line holds the leg's totals and the linger-window trap this script exists
  # for does not apply -- tail -1 is correct here, and only here. Absent on a
  # control leg by design (the line prints only with the switch on), and absent
  # on any leg missing -VoxelAssetDir, which makes the whole feature an empty
  # branch -- an armed leg with no line here measured nothing.
  #
  # FAILING READINGS, so a healthy-looking line cannot lie unchallenged:
  #   hits ~= inline            -> residency gate refusing to store (or the cache
  #                                is not being consulted); expect ~8:1 warm.
  #   worker=0.0 ms with coarse -> warm pass is a silent no-op; the only win is
  #   chunks dispatching           the cache. Level-0-only traffic is expected.
  #   raced ~= landed           -> warm work was never on the critical path.
  #   hits/(hits+inline) ~ 8/9  -> the cache is lying about what the resolve
  #   but cycPerColumn unmoved     cost; distrust the whole leg.
  b3=$(grep -o "assets RESOLVE (B\.3): .*" "$log" | tail -1)
  if [ -n "$b3" ]; then printf "%-18s   %s\n" "" "$b3"; fi
  # P3 worklist spine (-VoxelGpuWorklist). The window line prints only when
  # something happened THIS WINDOW (the quiet gate compares windowed skip
  # deltas as of 2026-08-23 -- before that it compared the cumulative skip
  # total, so every post-flight linger window printed zeros forever and
  # tail -1 here read "passes/tick mean=0.0 max=0" on legs whose active
  # windows read 70+; builds older than that fix still do this). tail -1 is
  # the last ACTIVE window. skips= and proof counters inside the line are
  # cumulative; passes/tick and records are windowed. Absent on a control
  # leg by design -- an armed leg with no line here promoted no jobs at all.
  # A "PASS TALLY DEAD" marker inside the line means the passes/tick number
  # measured nothing; treat every pass-count conclusion from the leg as void.
  #
  # FAILING READINGS (the full list is on MaybeLogWorklistWindow):
  #   proofFAILs > 0 or identity DRIFT -> the leg is INVALID, full stop.
  #   appended=0 with chunks flowing   -> spine carries no traffic; read the
  #                                       skips= reasons for which gate.
  #   proof landed=0 with consumption  -> GPU consumption UNVERIFIED; treat
  #                                       the spine as dead, not as quiet.
  # Last ACTIVE window: drop zero-chunk zero-record lines first (belt and
  # braces for logs from builds predating the windowed quiet gate), fall back
  # to the plain last line only if every window was idle.
  wl=$(grep -o "\[gpu-worklist\] [0-9.]*s window: .*" "$log" | grep -v "(0 chunks, 0 chunks/s); records appended=0" | tail -1)
  [ -n "$wl" ] || wl=$(grep -o "\[gpu-worklist\] [0-9.]*s window: .*" "$log" | tail -1)
  if [ -n "$wl" ]; then
    printf "%-18s   last:  %s\n" "" "$wl"
    # AND THE PEAK-RATE WINDOW, because "last active" is not "representative".
    # The 2026-08-23 worklist leg's last active window carried 2,010 chunks at
    # 402 chunks/s while the flight's own windows carried 29,796 at 5,941 --
    # and passes/tick read 36.6 there against 1,231.6 mid-flight. The drain
    # tail is an ACTIVE window by every test this script applies, so it passed
    # the quiet gate and was read as the leg's answer: a 34x understatement of
    # the one number the whole programme is gated on. Printing both means no
    # single line can quietly be the tail again. Read them as a PAIR; they
    # should be close, and when they are not, the peak one is the flight.
    wlpeak=$(grep -o "\[gpu-worklist\] [0-9.]*s window: .*" "$log" \
             | sed 's/.*(\([0-9]*\) chunks, .*/\1 &/' \
             | sort -k1,1nr | head -1 | cut -d" " -f2-)
    if [ -n "$wlpeak" ] && [ "$wlpeak" != "$wl" ]; then
      printf "%-18s   peak:  %s\n" "" "$wlpeak"
    fi
    # Converted-stage lines (cumulative counters, so the LAST line is the
    # leg's total -- tail -1 is correct here). One per armed stage; absent on
    # spine-only and control legs by design. Their FAILING READINGS live on
    # MaybeLogWorklistWindow.
    # wlclaim was MISSING from this list while stage 6 was the whole point of
    # the leg -- the stage that takes per-chunk passes to zero had no line in
    # the summary at all, and its set-identity triple (conv / hostStaged /
    # gpuClaimed) is what catches a double claim.
    for stage in wlcols wlvox wlct wlstamp wlpack wlclaim; do
      sl=$(grep -o "\[gpu-worklist\] $stage .*" "$log" | tail -1)
      if [ -n "$sl" ]; then printf "%-18s   %s\n" "" "$sl"; fi
    done
    wlfails=$(grep -c "\[gpu-worklist\] proof .* FAIL" "$log")
    if [ "$wlfails" -gt 0 ]; then
      printf "%-18s   worklist: %s PROOF FAILURES -- LEG INVALID\n" "" "$wlfails"
    fi
  fi
done
