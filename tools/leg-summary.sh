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
  # HOLES: PEAK **AND** LAST, and the peak is the one that means anything.
  #
  # THIS LINE USED TO BE `grep ... | tail -1` -- the exact "last:" read the
  # header of this very script exists to forbid, in the one counter where the
  # last window is guaranteed to be the least interesting. The flight ends,
  # LingerSec seconds of PARKED pose follow, and a parked camera has had time
  # to cover everything it can see, so the tail reads ~0 by construction. It is
  # not a measurement of coverage; it is a measurement of having stopped.
  #
  # WHAT IT COST. The D-stock/D-armed pair was read through this line and
  # reported `holes 0 -> 10`, i.e. "arming the GPU-primary set introduces
  # holes". That number went into a source comment as an owner-visible-defect
  # regression and into a decision the owner was asked to make. Read over the
  # FLIGHT windows instead, the same two logs say:
  #
  #     leg       last window   peak in-flight window
  #     D-stock   0             8382   of 30168 scanned
  #     D-armed   10            1859   of 30154 scanned
  #
  # The sign REVERSES: armed has ~4.5x FEWER holes while actually flying, which
  # is the regime the owner complains about ("whenever the player flies
  # forward"). The armed leg's residual 10 are a frozen parked-pose residue --
  # the same 8 R2 columns at r=511-512 m, cause=no-record, byte-identical in
  # all 29 linger windows.
  #
  # So this is the SECOND time this script has been the source of the trap it
  # was written to prevent (the first was passes/tick, wrong by 33x, fixed by
  # adding last:/peak: to the other counters -- and holes was simply missed).
  # Printing both is the fix, because for holes the two numbers answer
  # different questions and BOTH get asked: peak = "did the player ever see
  # through the world", last = "did it converge once parked". A leg that ends
  # with holesLast>0 has a genuine unadmitted residue worth a coordinate dump;
  # a leg with a large holesPeak had visible gaps in flight even if it tidied
  # up afterwards.
  # THE GLOBAL PEAK IS ALSO THE WRONG WINDOW, for the opposite reason to last:.
  #
  # Window 1 is taken before anything has streamed, so EVERY leg peaks at
  # ~27,000 holes there regardless of configuration -- it measures having just
  # started, exactly as `last:` measures having stopped. Reading the global peak
  # on the D-pair gives 27,498 vs 27,197: a 1% difference, which reads as "the
  # configuration makes no difference to holes" and buries the finding.
  #
  # The regime that matters is FLIGHT, and its boundary needs no hand-picked
  # index: the initial fill is over the first time coverage converges. So peak
  # AFTER first convergence is the flight peak, and it is reproducible.
  # On the D-pair that rule yields:
  #
  #     leg       fill peak   FLIGHT peak   flight mean   last
  #     D-stock   27498       8382          2651          0
  #     D-armed   27197       1859           721         10
  #
  # Armed has 4.5x FEWER holes in flight and 3.7x fewer on average -- the regime
  # the owner complains about ("whenever the player flies forward"). Its
  # residual 10 is a frozen parked-pose residue: the same 8 R2 columns at
  # r=511-512 m, cause=no-record, byte-identical across all 29 linger windows.
  #
  # Read holesFlight for "did the player ever see through the world while
  # moving", and holesLast for "did it converge once parked". Both get asked and
  # they can disagree in sign, which is how `holes 0 -> 10` reached a source
  # comment and an owner decision as a regression when flight says the reverse.
  holes=$(grep -o 'holes=[0-9]* scanned=[0-9]*' "$log" | awk '
    { split($0, a, /[= ]/); h[NR] = a[2] + 0; sc[NR] = a[4] + 0 }
    END {
      if (NR == 0) { printf "holes=UNARMED(no coverage line -- this leg cannot report a hole)"; exit }
      conv = 0
      for (i = 1; i <= NR; i++) if (h[i] <= 10) { conv = i; break }
      fill = 0
      for (i = 1; i <= (conv ? conv : NR); i++) if (h[i] > fill) fill = h[i]
      fpk = 0; fsum = 0; fn = 0
      if (conv) for (i = conv + 1; i <= NR; i++) { if (h[i] > fpk) fpk = h[i]; fsum += h[i]; fn++ }
      printf "holesFill=%d holesFlight=%d", fill, fpk
      if (fn) printf " holesFlightMean=%.0f", fsum / fn
      printf " holesLast=%d/%d", h[NR], sc[NR]
      if (!conv) printf " (NEVER CONVERGED -- flight window undefined)"
    }')
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
