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
echo "=== 4. THE TRAFFIC. families/frame COUNTS DISTINCT RDG GRAPHS, NOT Touch()"
echo "===    CALLS -- it counted calls until 2026-08-25 and therefore read 2.00"
echo "===    on EVERY leg on disk (5,614 lines, never any other value), which"
echo "===    failed D0 unconditionally and made the whole breakdown unquotable."
echo "===    touches/frame is printed beside it: 1.00-3.00 touches against 1.00"
echo "===    families is the HEALTHY reading, not a defect. families/frame above"
echo "===    1.01 means a second graph EXECUTED inside the A->B span and the"
echo "===    split is not a partition; camSpeedMS near zero on a MOVING line"
echo "===    means the leg is invalid, not fast; dropped is frames this file"
echo "===    refused to guess at. Every rate carries its own numerator and"
echo "===    denominator on the line. ==="
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

echo
echo "=== 7. TAIL ATTRIBUTION (-VoxelRenderFrame=2 only; empty at level 1 is"
echo "===    CORRECT, not missing). h= is the traffic counter and it comes"
echo "===    first: h=0 with ms=0.000 is a DEAD SCOPE or a subsystem that did"
echo "===    not run; h>0 with ms=0.000 is a group that ran and cost nothing."
echo "===    Only the second may be reported as cheap. ==="
grep "Voxel render frame seg=SETTLED-MOVING-LEG TAIL " "$LOG" | tail -1
grep "Voxel render frame seg=SETTLED-PARKED-LEG TAIL " "$LOG" | tail -1
grep -m1 "TAIL-READING" "$LOG"

echo
echo "=== 7b. WHICH h=0 IS WHICH, from the build rather than from a guess."
echo "===    18 of the 29 sites are wired (chunkIndex 2, residency 3, poolComp"
echo "===    5, giVol 8); meshJob 7 and brickPool 4 are UNWIRED. An UNWIRED"
echo "===    group's h=0 says NOTHING about that subsystem -- it says this build"
echo "===    has no scope for it. While groupsUnwired>0 the section-8 negative"
echo "===    verdict is NOT DECIDABLE. tableCheck=TABLE-LIES means the table is"
echo "===    stale: trust the hits. ==="
grep "Voxel render frame seg=SETTLED-MOVING-LEG TAIL-WIRING" "$LOG" | tail -1
grep "Voxel render frame seg=SETTLED-PARKED-LEG TAIL-WIRING" "$LOG" | tail -1

echo
echo "=== 8. WHICH SUBSYSTEM D4 IS. If dTailOther carries most of dTail the 29"
echo "===    instrumented sites are NOT the mechanism and no group may be named"
echo "===    as it -- BUT ONLY IF groupsUnwired=0 in section 7b. Read"
echo "===    l2OverheadMs on the TAIL line before quoting a tailMs from a"
echo "===    level-2 leg against one from a level-1 leg. ==="
grep "Voxel render frame DELTA-TAIL tag=moving-vs-parked-LEG" "$LOG" | tail -1

echo
echo "=== 9. THE D0 GATE'S OWN RED ARM. A gate only ever observed passing is"
echo "===    not a gate, and this one was only ever observed FAILING. Run one"
echo "===    leg with -VoxelRenderFrameFakeFamilies=2: it MUST print"
echo "===    families/frame=2.00 and VERDICT=D0-FAILED. If a fake-family leg"
echo "===    still reads 1.00, families/frame has become a constant and the"
echo "===    numbers under it are unproven. ==="
grep -m1 "Voxel render frame ARMED" "$LOG" | grep -o "fakeFamilies=[0-9]*" || echo "  (armed line has no fakeFamilies field -- stale binary)"
