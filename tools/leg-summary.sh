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
done
