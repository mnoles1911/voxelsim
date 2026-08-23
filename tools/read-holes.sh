#!/usr/bin/env bash
# Read the level-2 hole breakdown out of a capture/leg log.
#
# WHY A SCRIPT. Three things in this line silently invalidate it, and all three
# are easy to scroll past:
#   * "[ANNOTATION WRITER DISARMED ...]" -- the reason split is STALE BITS.
#     Only the level histogram means anything. Reading never/pending/evicted
#     off such a window is reading noise as a finding.
#   * "[WINDOW MIXED ARM LEVELS ...]" -- attributed covers fewer frames than
#     uncovered, so the two are not comparable.
#   * "[SHORTFALL: ...]" -- the capture missed rays. An instrument defect.
# This prints every window with the flags made loud, rather than the last one.
#
# It prints ALL windows deliberately. tools/leg-summary.sh exists because
# `grep | tail -1` lands on the post-flight linger window, which reads as all
# zeros and has already produced two retracted readings on this project.
set -u
for f in "$@"; do
  echo "############ $f"
  grep -o "Voxel march holes breakdown.*" "$f" | awk '
    /ARMED BUT NO LEVEL-2 FRAMES/ { print "  !! " $0; next }
    /NOT MEASURED at HoleStats 1/ { print "  !! " $0; next }
    /!=/                          { print "  !! DISCARD: " $0; next }
    {
      flag=""
      if (index($0,"ANNOTATION WRITER DISARMED")) flag=flag" [REASONS ARE STALE -- LEVELS ONLY]"
      if (index($0,"WINDOW MIXED ARM LEVELS"))    flag=flag" [MIXED ARM -- NOT COMPARABLE]"
      if (index($0,"SHORTFALL"))                  flag=flag" [SHORTFALL -- INSTRUMENT DEFECT]"
      print "  " $0 flag
    }'
  echo
done
