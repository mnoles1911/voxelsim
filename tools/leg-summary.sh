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
  disp=$(grep -o "job flow (5s window): dispatched=[0-9]*" "$log" | awk -F= '{s+=$NF} END{printf "%d",s}')
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
done
