#!/usr/bin/env python3
"""Split a flight leg into PREFLIGHT / FLYING / LINGER and cost each phase.

    python tools/flying-segment.py Saved/leg.log [more.log ...]

WHY THIS EXISTS. The owner's gate is about the MOVING player: steady FPS above
100 at 20 m/s. Parked is already fine. So a leg-wide average answers the wrong
question -- it mixes a cold fill, a flight, and a 60 s parked linger in which
the streaming tick does almost nothing, and every one of those has a different
cost shape.

Measured on ahead-on.log, same build, same settled world, only the anchor
moving:

    PREFLIGHT   perTick tick  2.07 ms   recompute 0.72 ms   35% of tick
    FLYING      perTick tick 11.93 ms   recompute 8.49 ms   71% of tick
    LINGER      perTick tick  0.16 ms   recompute 0.06 ms

Recompute is 12x more expensive per tick while flying than while cold-filling,
and ~140x more than parked. That is the flying-vs-parked delta, and reading it
leg-wide hides it completely.

THE PHASE BOUNDARIES ARE READ FROM THE LOG, NOT ASSUMED. UVoxelPerfRunSubsystem
prints `preflight warmup for N s` and `flight ended at t=...`, so the split is
the run's own clock. A leg missing either marker is reported as such rather
than sliced on a guessed offset.

SAY WHICH WINDOW YOU READ. Every per-tick figure here divides by the tick count
inside the phase, so it is window-length independent -- which matters because
the `(5s window)` label in 40 log lines is wrong on any leg passing
-VoxelPerfLogInterval=2 (measured median gap 2.01 s). Rates per SECOND here use
the phase's real span from the timestamps, never a nominal interval.

TWO PARTS TO THE GATE, AND BOTH ARE PRINTED. p95 is one; hitches are the other.
A 10 ms p95 with 40 hitches per 5 s is a failure -- a hitch is a visible stutter
whatever the percentile says. Hitch COUNT and recompute's share OF a hitch
frame are therefore reported beside the means, because a mean cannot fail that
half of the gate.
"""

import re, io, sys

TS = re.compile(r'\[\d{4}\.\d\d\.\d\d-(\d\d)\.(\d\d)\.(\d\d):(\d\d\d)\]')
TB = re.compile(r'Voxel tick budget \(5s window\): ticks=(\d+) tickMs=([\d.]+)')
SUM = re.compile(r'Voxel recompute \(sum since last log\): totalMs=([\d.]+) fineMs=([\d.]+) '
                 r'exitScanMs=([\d.]+) queueFilterMs=([\d.]+) sortMs=([\d.]+) \| entryMs (.*)')
HR = re.compile(r'Hitch frame recompute: recomputeMs=([\d.]+)')
PRE = re.compile(r'VoxelPerfRun: preflight warmup for ([\d.]+)s')
END = re.compile(r'VoxelPerfRun: flight ended at t=([\d.]+)s')
RN = re.compile(r'R(\d)=([\d.]+)')


def secs(m):
    h, mi, s, ms = [int(x) for x in m.groups()]
    return h * 3600 + mi * 60 + s + ms / 1000.0


def load(path):
    tb, sm, hits = [], [], []
    t_pre = None
    pre_len = 90.0
    t_end = None
    for line in io.open(path, errors='ignore'):
        mt = TS.search(line)
        if not mt:
            continue
        t = secs(mt)
        m = PRE.search(line)
        if m and t_pre is None:
            t_pre, pre_len = t, float(m.group(1))
            continue
        m = END.search(line)
        if m:
            t_end = t
            continue
        m = HR.search(line)
        if m:
            hits.append((t, float(m.group(1))))
            continue
        m = TB.search(line)
        if m:
            tb.append((t, int(m.group(1)), float(m.group(2))))
            continue
        m = SUM.search(line)
        if m:
            ent = sum(float(v) for _, v in RN.findall(m.group(6)))
            sm.append((t, float(m.group(1)), float(m.group(3)), ent, float(m.group(5))))
    return tb, sm, hits, t_pre, pre_len, t_end


def phase(name, tb, sm, hits, lo, hi):
    T = [x for x in tb if lo <= x[0] < hi]
    S = [x for x in sm if lo <= x[0] < hi]
    H = [x for x in hits if lo <= x[0] < hi]
    if not T:
        print('   %-10s no windows' % name)
        return
    span = hi - lo
    ticks = sum(x[1] for x in T)
    tickms = sum(x[2] for x in T)
    tot = sum(x[1] for x in S)
    ex = sum(x[2] for x in S)
    en = sum(x[3] for x in S)
    if ticks == 0:
        print('   %-10s ticks=0' % name)
        return
    print('   %-9s span=%5.0fs ticks=%-6d rate=%5.1f/s | tick=%6.2f ms/tick  '
          'recompute=%6.2f (%2.0f%%)  entry=%5.2f  exit=%5.2f'
          % (name, span, ticks, ticks / span, tickms / ticks, tot / ticks,
             100 * tot / tickms if tickms else 0, en / ticks, ex / ticks))
    if H:
        v = sorted(x[1] for x in H)
        print('             hitches=%-5d %5.1f/s | recomputeMs ON a hitch frame: '
              'mean=%5.1f p50=%5.1f p95=%5.1f max=%6.1f'
              % (len(H), len(H) / span, sum(v) / len(v), v[len(v) // 2],
                 v[int(len(v) * 0.95)], v[-1]))
    else:
        print('             hitches=0 in this phase')


def main(paths):
    for p in paths:
        tb, sm, hits, t_pre, pre_len, t_end = load(p)
        if not tb:
            print('== %s  no `Voxel tick budget` lines' % p)
            continue
        print('== %s' % p)
        if t_pre is None:
            print('   NO preflight marker: not a scripted flight leg. Phases cannot be split,')
            print('   and a leg-wide mean would answer the wrong question. Refusing to slice.')
            continue
        f0 = t_pre + pre_len
        f1 = t_end if t_end is not None else tb[-1][0] + 0.001
        if t_end is None:
            print('   NO `flight ended` marker: the FLYING phase is open-ended and includes')
            print('   whatever followed. Treat its numbers as contaminated.')
        phase('PREFLIGHT', tb, sm, hits, t_pre, f0)
        phase('FLYING', tb, sm, hits, f0, f1)
        last = max(tb[-1][0], hits[-1][0] if hits else tb[-1][0])
        phase('LINGER', tb, sm, hits, f1, last + 0.001)
        print('')
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
