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
# A FOURTH, ADDED 2026-08-23, AND IT IS THE ONE THAT COST A NIGHT. The line
# now prints "uncShell=N (P% of uncovered=M)". uncShell is the hole count --
# absent chunks that touch resident ground. `uncovered` is every absent
# crossing, and on a settled stationary world it read 25.27% OF ALL RAYS while
# the picture never changed, because the streamed set is a shell and the camera
# flies in the air outside it. So:
#   * P == 100.00%  -- the shell test narrowed NOTHING. Either the index bind
#     is empty (MarchIndexLevelPopulated = 0 makes every absent chunk a hole)
#     or the build predates the fix. The histograms are back to describing air;
#     do not quote them.
#   * byLevel all at L0 -- the pre-fix signature. It was L0=100% in EVERY
#     window of all three zcut rungs because segment 0 starts at the camera and
#     the camera flies in absent air. If it comes back, so has the defect.
# Both are flagged below rather than left to be noticed.
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
      # The shell test narrowed nothing: uncShell == uncovered.
      if (match($0,/\(100\.00% of uncovered/))    flag=flag" [SHELL TEST NARROWED NOTHING -- HISTOGRAMS DESCRIBE AIR]"
      # The pre-2026-08-23 signature: every attributed ray at L0.
      if (match($0,/L1=0 L2=0 L3=0 L4=0 L5=0 L6=0/) && !match($0,/byLevel L0=0 /))         flag=flag" [ALL MASS AT L0 -- THE CAMERA-AIR DEFECT, DO NOT QUOTE]"
      print "  " $0 flag
    }'
  echo
done
