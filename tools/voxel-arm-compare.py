#!/usr/bin/env python3
"""Compare two capture CSVs counter by counter, over a NAMED population.

WHY THIS EXISTS. Every arm this project runs ends in the same act: take two
frames.csv files that differ by one cvar, and ask which counters moved. That was
being done with an ad-hoc script each time, and ad-hoc scripts are where the
reading rules get forgotten -- which population, which statistic, and whether
the counter that was supposed to move actually exists in both files.

THE THREE RULES IT ENFORCES, each learned by being got wrong here:

  * MEDIANS, NEVER MEANS. Every streaming counter in this project is bimodal:
    SubmitMs reads p50 0.00 with a max over a second. A mean of a bimodal
    counter is a number about neither mode.

  * A MISSING COUNTER IS VOID, NOT ZERO. If a counter named on the command line
    is absent from either file, this refuses rather than printing 0.000 and a
    tidy -100%. An absent column is what a disabled stat group looks like, and
    reading it as "the cost went away" is this project's house failure.

  * THE POPULATION IS NAMED ON EVERY ROW. `--standing` and `--moving` split on
    VoxelStream/SpeedMps, which is what the pawn actually did, rather than on a
    frame index that assumes the phase order. `--frames A:B` keeps the old
    index window for comparability with records that quote it.

The game-thread twin of tools/csv-gpu-attrib.py, which owns the GPU side.
"""
import argparse
import csv
import statistics as st
import sys

# The counters worth printing when none are named: the frame's three clocks,
# the streaming tick and its parts, and the collision/water items that the
# 2026-09-11 work made the largest terms on the quiet game thread.
DEFAULT_COUNTERS = [
    'FrameTime', 'GameThreadTime', 'RenderThreadTime', 'GPUTime',
    'VoxelStream/TickMs', 'VoxelStream/DispatchMs', 'VoxelStream/SubmitMs',
    'VoxelStream/CollisionPrepareMs', 'VoxelStream/CollisionPreparations',
    'VoxelStream/GameThread/RoofProbeMs', 'VoxelStream/GameThread/ClipmapTickMs',
    'VoxelStream/GameThread/OceanTickMs', 'VoxelStream/GameThread/RippleTickMs',
    'VoxelStream/GameThread/PawnTickMs', 'VoxelStream/UnderwaterQueryCalls',
    'GPU/VoxelMarch', 'GPU/Basepass', 'GPU/RenderVelocities',
]


def load(path):
    with open(path, newline='', encoding='utf-8-sig') as handle:
        return list(csv.DictReader(handle))


def number(row, key):
    try:
        return float(row.get(key, ''))
    except (TypeError, ValueError):
        return None


def select(rows, args):
    """The population, and the one-line description that goes on the report."""
    if args.frames:
        lo, hi = (int(part) for part in args.frames.split(':'))
        return rows[lo:hi], f'frames {lo}:{hi}'
    if args.standing or args.moving:
        picked = []
        for row in rows:
            speed = number(row, 'VoxelStream/SpeedMps')
            if speed is None:
                continue
            if args.moving and speed > 0.1:
                picked.append(row)
            elif args.standing and speed <= 0.1:
                picked.append(row)
        if not picked:
            sys.exit('REFUSING: no frames matched the requested population -- '
                     'VoxelStream/SpeedMps is absent or never left the other state. '
                     'A population of zero frames is void, not an answer.')
        return picked, ('moving frames (SpeedMps > 0.1)' if args.moving
                        else 'standing frames (SpeedMps <= 0.1)')
    return rows, 'all frames'


def percentiles(values):
    ordered = sorted(values)
    if not ordered:
        return None
    def at(fraction):
        return ordered[min(len(ordered) - 1, int(len(ordered) * fraction))]
    return at(0.5), at(0.95), at(0.99), ordered[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('control')
    parser.add_argument('arm')
    parser.add_argument('--counters', nargs='*', default=None,
                        help='counter columns to compare; default is the standard set')
    parser.add_argument('--frames', default=None, metavar='A:B',
                        help='index window, e.g. 0:200')
    parser.add_argument('--standing', action='store_true')
    parser.add_argument('--moving', action='store_true')
    parser.add_argument('--label-control', default='control')
    parser.add_argument('--label-arm', default='arm')
    args = parser.parse_args()
    if args.standing and args.moving:
        sys.exit('--standing and --moving are different populations; pick one')

    control_rows, arm_rows = load(args.control), load(args.arm)
    control_pop, description = select(control_rows, args)
    arm_pop, _ = select(arm_rows, args)

    counters = args.counters if args.counters else DEFAULT_COUNTERS
    named = bool(args.counters)

    print(f'population : {description}')
    print(f'{args.label_control:<10} {len(control_pop)} of {len(control_rows)} frames   {args.control}')
    print(f'{args.label_arm:<10} {len(arm_pop)} of {len(arm_rows)} frames   {args.arm}')

    for label, pop in ((args.label_control, control_pop), (args.label_arm, arm_pop)):
        stats = percentiles([v for v in (number(r, 'FrameTime') for r in pop) if v is not None])
        if stats:
            print(f'  {label:<8} FrameTime  p50 {stats[0]:7.2f}  p95 {stats[1]:7.2f}  '
                  f'p99 {stats[2]:7.2f}  max {stats[3]:8.2f}')

    print()
    print(f'{"counter":<44} {args.label_control:>11} {args.label_arm:>11} {"delta":>10} {"change":>9}')
    print('-' * 90)
    missing = []
    for counter in counters:
        control_values = [v for v in (number(r, counter) for r in control_pop) if v is not None]
        arm_values = [v for v in (number(r, counter) for r in arm_pop) if v is not None]
        if not control_values or not arm_values:
            # Absent in one side. Named explicitly -> refuse at the end; part of
            # the default set -> skip quietly, since the default set spans
            # several binaries' worth of counters.
            if named:
                missing.append(counter)
            continue
        control_median, arm_median = st.median(control_values), st.median(arm_values)
        delta = arm_median - control_median
        change = (100.0 * delta / control_median) if control_median else float('nan')
        print(f'{counter:<44} {control_median:11.3f} {arm_median:11.3f} {delta:+10.3f} {change:+8.1f}%')

    if missing:
        print()
        sys.exit('REFUSING TO REPORT: these counters were named but are absent from one or both '
                 'captures, and an absent column is not a zero: ' + ', '.join(missing))


if __name__ == '__main__':
    main()
