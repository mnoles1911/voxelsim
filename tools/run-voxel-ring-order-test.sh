#!/usr/bin/env bash
# Compile and run the standalone checks for VoxelRingOrder.h and
# VoxelEditedLaneGate.h.
#
# WHY THIS EXISTS. Both headers rest on ONE geometric inequality -- a chunk
# centre is within half a diagonal of the anchor's own chunk centre -- and
# every bound in them is derived from it. A geometric claim can be checked
# exactly, against brute force, with no build lane, no editor and no leg. So it
# is, before either mechanism is proposed to anybody.
#
# It compiles THE ENGINE HEADERS, not a copy. tools/test-voxel-ring-order.cpp
# includes them by relative path; a test that re-derived the ordering would be
# testing itself, which is how three green probes in one night came to measure
# a world the engine was not running.
#
# CONFIRMED TO FAIL WHEN MUTATED (2026-08-23): deleting the half-diagonal slack
# from StopRadiusSqAfterLatch and from WindowFor's inner edge produces 1,102
# failures across R3 and R4. A check that has only ever been seen to pass is
# not evidence.

set -u
cd "$(dirname "$0")/.."

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
  for C in clang++ g++ c++; do
    if command -v "$C" >/dev/null 2>&1; then CXX="$C"; break; fi
  done
fi
if [ -z "$CXX" ]; then
  echo "no C++ compiler on PATH (tried clang++, g++, c++); set CXX=" >&2
  exit 2
fi

OUT="${TMPDIR:-/tmp}/voxel-ring-order-test.exe"
"$CXX" -std=c++17 -O2 -Wall -Wextra \
  -o "$OUT" \
  tools/test-voxel-ring-order.cpp \
  ue-project/Source/VoxelEarth/VoxelRingOrder.cpp || exit 1

"$OUT"
