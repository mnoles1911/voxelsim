#!/usr/bin/env bash
# Read the render-frame split out of a leg log, in the order that stops the
# five wrong readings this project has already paid for.
#
#   tools/read-render-frame.sh Saved/rf-1.log
#
# WHY A READER EXISTS AT ALL. `grep | tail -1` on a flight leg lands on the
# POST-FLIGHT LINGER window, which is PARKED, and reads a 9 ms render frame as
# though it described the flight. That has produced four retractions here, and
# a fifth came from a summary tool that printed only its own last window. So
# this prints, in order: whether the instrument was armed, whether it is valid,
# the LEG totals for both segments, and the verdict -- and it refuses to be
# read as a pass when the moving population is empty.
set -u
LOG="${1:-}"
if [ -z "$LOG" ] || [ ! -f "$LOG" ]; then
  echo "usage: tools/read-render-frame.sh <leg.log>" >&2
  exit 2
fi

echo "=== 1. WAS IT ARMED? (no line here means -VoxelRenderFrame was not passed,"
echo "===    or neither the marcher nor the fluid extension registered) ==="
grep -m1 "Voxel render frame ARMED" "$LOG" || echo "  NOT ARMED -- nothing below exists."

echo
echo "=== 2. THE LEG TOTALS. These are the lines to quote. The owner's gate is"
echo "===    SETTLED-MOVING; SETTLED-PARKED may never be quoted in its place. ==="
grep "Voxel render frame seg=SETTLED-MOVING-LEG" "$LOG" | tail -3
grep "Voxel render frame seg=SETTLED-PARKED-LEG" "$LOG" | tail -3

echo
echo "=== 3. THE VERDICT against the disproof registered in VoxelRenderFrame.h"
echo "===    BEFORE the leg ran. D0=FAILED means nothing else may be quoted. ==="
grep "Voxel render frame DELTA tag=moving-vs-parked-LEG" "$LOG" | tail -1

echo
echo "=== 4. THE TRAFFIC. families/frame above 1.01 means the split is not a"
echo "===    partition; camSpeedMS near zero on a MOVING line means the leg is"
echo "===    invalid, not fast; dropped is frames this file refused to guess at. ==="
grep "Voxel render frame seg=SETTLED-MOVING-LEG TRAFFIC" "$LOG" | tail -1
grep "Voxel render frame seg=SETTLED-PARKED-LEG TRAFFIC" "$LOG" | tail -1

echo
echo "=== 5. PER-WINDOW MOVING, so a reader can see which regime a number came"
echo "===    from rather than trusting one aggregate. The LAST window of a"
echo "===    flight leg is the LINGER and it is PARKED -- do not read it. ==="
grep "Voxel render frame seg=SETTLED-MOVING n=" "$LOG" | tail -8

echo
echo "=== 6. THE FRAME-PHASE LINE THIS SPLIT IS SPLITTING. If renderBusyMs here"
echo "===    and in section 2 disagree, one of the two instruments is describing"
echo "===    a different population and NEITHER may be quoted until that is"
echo "===    resolved. Same leg, same segment, same number, or stop. ==="
grep "Voxel frame phase PIPELINE seg=SETTLED-MOVING-LEG" "$LOG" | tail -1
grep "Voxel frame phase PIPELINE seg=SETTLED-PARKED-LEG" "$LOG" | tail -1
