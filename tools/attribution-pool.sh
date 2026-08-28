#!/usr/bin/env bash
# Pool `Voxel frame attribution` windows across legs, n-weighted.
#
# WHY THIS EXISTS. A single attribution window's TAIL bucket holds three to
# twelve frames. Reading one is not reading a population, and doing so once
# produced a clean, convincing, WRONG headline ("the tail is the GPU, the
# game-thread theory is dead") that pooling immediately reversed. This weights
# every window by its own n so the answer comes from every frame in every leg.
#
# Requires legs run with `voxel.Stream.FrameAttribution 2` (SETTLED-MOVING).
# Legs without the per-frame GPU clock have no gpu= field and will read 0.00 --
# check the GPU-CLOCK arm line before believing a gpu column.
#
# Usage:  tools/attribution-pool.sh Saved/HCAP-ctl-a.log Saved/HCAP-ctl-b.log
#         tools/attribution-pool.sh Saved/*.log
set -u
[ $# -eq 0 ] && { echo "usage: $0 <log>..." >&2; exit 2; }

cat "$@" 2>/dev/null | grep "Voxel frame attribution" | awk '
function g(s,k,  m){ m=""; if (match(s, k"=[0-9.]+")) { m=substr(s,RSTART,RLENGTH); sub(k"=","",m) } return m+0 }
function acc(c,  _){ }
/FAST\(n=/ { if (match($0,/FAST\(n=[0-9]+/)) c=substr($0,RSTART+7,RLENGTH-7)+0
  fw+=c; ff+=c*g($0,"frame"); fg+=c*g($0,"gameMs"); fq+=c*g($0," gpu"); fr+=c*g($0,"render")
  ft+=c*g($0,"tick"); fd+=c*g($0,"dispatch"); fs+=c*g($0,"submit"); fa+=c*g($0,"apply"); fc+=c*g($0,"appl") }
/SLOW\(n=/ { if (match($0,/SLOW\(n=[0-9]+/)) c=substr($0,RSTART+7,RLENGTH-7)+0
  sw+=c; sf+=c*g($0,"frame"); sg+=c*g($0,"gameMs"); sq+=c*g($0," gpu"); sr+=c*g($0,"render")
  st+=c*g($0,"tick"); sd+=c*g($0,"dispatch"); ss+=c*g($0,"submit"); sa+=c*g($0,"apply"); sc+=c*g($0,"appl") }
/TAIL\(n=/ { if (match($0,/TAIL\(n=[0-9]+/)) c=substr($0,RSTART+7,RLENGTH-7)+0
  tw+=c; tf+=c*g($0,"frame"); tg+=c*g($0,"gameMs"); tq+=c*g($0," gpu"); tr+=c*g($0,"render")
  tt+=c*g($0,"tick"); td+=c*g($0,"dispatch"); ts+=c*g($0,"submit"); ta+=c*g($0,"apply"); tc+=c*g($0,"appl") }
END {
  if (fw == 0) { print "no FAST rows -- were these legs run with voxel.Stream.FrameAttribution 2?"; exit 1 }
  printf "%-6s %10s %7s %7s %7s %7s %7s %8s %7s %7s %8s\n",
         "bucket","n","frame","gameMs","gpu","render","tick","dispatch","submit","apply","chunks"
  printf "%-6s %10d %7.2f %7.2f %7.2f %7.2f %7.3f %8.3f %7.3f %7.3f %8.1f\n",
         "FAST",fw,ff/fw,fg/fw,fq/fw,fr/fw,ft/fw,fd/fw,fs/fw,fa/fw,fc/fw
  printf "%-6s %10d %7.2f %7.2f %7.2f %7.2f %7.3f %8.3f %7.3f %7.3f %8.1f\n",
         "SLOW",sw,sf/sw,sg/sw,sq/sw,sr/sw,st/sw,sd/sw,ss/sw,sa/sw,sc/sw
  printf "%-6s %10d %7.2f %7.2f %7.2f %7.2f %7.3f %8.3f %7.3f %7.3f %8.1f\n",
         "TAIL",tw,tf/tw,tg/tw,tq/tw,tr/tw,tt/tw,td/tw,ts/tw,ta/tw,tc/tw
  print ""
  printf "THE TWO STEPS (gameMs and gpu are CONCURRENT -- they do not sum to frame)\n"
  printf "  FAST -> SLOW   gpu %+6.2f   game %+6.2f\n", sq/sw-fq/fw, sg/sw-fg/fw
  printf "  SLOW -> TAIL   gpu %+6.2f   game %+6.2f\n", tq/tw-sq/sw, tg/tw-sg/sw
  print ""
  printf "INSIDE THE GAME-THREAD RISE, FAST -> TAIL (submit is a SUBSET of dispatch)\n"
  printf "  gameMs %+.2f | voxel tick %+.2f (%.1f%%) | dispatch %+.2f | submit %+.2f | apply %+.2f\n",
         tg/tw-fg/fw, tt/tw-ft/fw, 100*(tt/tw-ft/fw)/(tg/tw-fg/fw), td/tw-fd/fw, ts/tw-fs/fw, ta/tw-fa/fw
  printf "  unnamed inside the tick: %+.2f ms   (tick rise minus dispatch and apply)\n",
         (tt/tw-ft/fw) - (td/tw-fd/fw) - (ta/tw-fa/fw)
  printf "  chunks applied: FAST %.1f  SLOW %.1f  TAIL %.1f", fc/fw, sc/sw, tc/tw
  if (tc/tw < sc/sw) printf "   <- TAIL applies FEWER than SLOW: not a volume problem"
  print ""
}'
