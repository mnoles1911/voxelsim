#!/usr/bin/env python3
"""Aggregate what RecomputeDesiredSet spends, from a leg log.

    python tools/recompute-census.py Saved/ahead-on.log [more.log ...]

TWO PASSES, AND THE FIRST ONE WORKS ON EVERY LOG SINCE 2026-08-23.

PASS 1 -- the stage split, from `Voxel recompute (sum since last log)`.
That line has existed since the stage timers were split and had NEVER been
aggregated; doing so is what established that entry scan plus exit walk are
90-99% of recompute and that the existing buckets already account for 99.94%
of it. The RESIDUAL is computed and printed here because the log line itself
does not print it -- see the hook in
docs/recompute-cost-census-2026-08-23.md for the one-line fix.

  The residual is SIGNED on purpose. Positive means a stage is unbucketed;
  NEGATIVE means two timers overlap and every share is double-counted. An
  absolute value would hide the second, which is the harder fault.

PASS 2 -- the per-operation split inside entryMs, from `Voxel recompute entry
census`, which only appears under -VoxelRecomputeCensus. The Z loop runs
~2.4 MILLION times per window, so it is COUNTED, never timed (a clock pair per
Z cell would add ~15% to the number being measured). The per-operation costs
are recovered by least squares over levels and windows: the six rings have
very different mixes, so a leg supplies ~500 (entryMs, counts) observations.

  THE FIT'S RESIDUAL IS THE RECONCILIATION DELTA AND IT IS PRINTED. If the
  model cannot explain entryMs from the counters, that shows up as a large
  residual rather than as plausible-looking shares. A NEGATIVE fitted cost is
  reported as a failure too -- an operation cannot take negative time, so it
  means the counters are collinear and the split is not identifiable from this
  leg. In either case the shares must not be read.

No third-party dependency: the normal equations are solved with Gaussian
elimination on a 5x5, which is what the problem is.
"""

import re
import sys

STAGE = re.compile(
    r'Voxel recompute \(sum since last log\): totalMs=([\d.]+) fineMs=([\d.]+) '
    r'exitScanMs=([\d.]+) queueFilterMs=([\d.]+) sortMs=([\d.]+) \| entryMs (.*)')
ENTRY = re.compile(r'R(\d)=([\d.]+)')

# -VoxelRecomputeCensus, one per level per window. Field order matches the hook.
CENSUS = re.compile(
    r'Voxel recompute entry census .*?R(\d)\[ms=([\d.]+) cells=(\d+) incrSkip=(\d+) '
    r'geoRej=(\d+) memoHit=(\d+) memoFill=(\d+) memoFillMs=([\d.]+) zCells=(\d+) '
    r'recProbe=(\d+) parkProbe=(\d+) admit=(\d+) defer=(\d+)\]')

TERMS = [
    ('cells',    'per footprint visited (geometry + memo lookup)'),
    ('zCells',   'per Z cell (two hash probes + the sort key)'),
    ('memoFill', 'per memo fill (an amplifier column)'),
    ('admit',    'per admission (record insert + queue push)'),
    ('defer',    'per deferral bookkeeping op'),
]


def solve(A, b):
    """Gaussian elimination with partial pivoting. Returns None if singular."""
    n = len(b)
    M = [row[:] + [b[i]] for i, row in enumerate(A)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(M[r][c]))
        if abs(M[p][c]) < 1e-18:
            return None
        M[c], M[p] = M[p], M[c]
        for r in range(n):
            if r == c:
                continue
            f = M[r][c] / M[c][c]
            for k in range(c, n + 1):
                M[r][k] -= f * M[c][k]
    return [M[i][n] / M[i][i] for i in range(n)]


def pass1(path):
    tot = fine = ex = qf = so = 0.0
    ent = [0.0] * 7
    n = 0
    rmin, rmax = 0.0, 0.0
    series = []
    for line in open(path, errors='ignore'):
        m = STAGE.search(line)
        if not m:
            continue
        t = float(m.group(1))
        if t <= 0.0:
            continue  # linger window: no recompute ran. Not a measurement.
        n += 1
        f, x, q, s = (float(m.group(i)) for i in (2, 3, 4, 5))
        es = [0.0] * 7
        for lv, v in ENTRY.findall(m.group(6)):
            es[int(lv)] = float(v)
        r = t - f - x - q - s - sum(es)
        rmin, rmax = min(rmin, r), max(rmax, r)
        tot += t; fine += f; ex += x; qf += q; so += s
        for i in range(7):
            ent[i] += es[i]
        series.append((t, sum(es), x))
    if n == 0:
        print('  no active windows (every totalMs was 0 -- linger only)')
        return
    esum = sum(ent)
    resid = tot - fine - ex - qf - so - esum
    pc = lambda v: 100.0 * v / tot if tot else 0.0
    print('  PASS 1  stage split -- %d active windows, %.1f ms/window' % (n, tot / n))
    print('     entryScan     %9.0f ms  %5.1f%%   the annulus sweeps' % (esum, pc(esum)))
    for i in range(7):
        if ent[i] > 0:
            print('        R%d         %9.0f ms  %5.1f%%' % (i, ent[i], pc(ent[i])))
    print('     exitScan      %9.0f ms  %5.1f%%   the O(ChunkRecords) walk' % (ex, pc(ex)))
    print('     sort          %9.0f ms  %5.1f%%' % (so, pc(so)))
    print('     fineResidency %9.0f ms  %5.1f%%' % (fine, pc(fine)))
    print('     queueFilter   %9.0f ms  %5.1f%%' % (qf, pc(qf)))
    print('     RESIDUAL      %9.1f ms  %5.2f%%   signed; per-window %.2f .. %.2f'
          % (resid, pc(resid), rmin, rmax))
    if rmin < -0.5:
        print('     ** NEGATIVE residual: two stage timers OVERLAP. Every share above')
        print('        is double-counted somewhere. Do not read them. **')
    elif pc(resid) > 5.0:
        print('     ** residual over 5%: a stage is missing a bucket. **')

    # Does it rise through the run, and which half?
    q4 = len(series) // 4
    if q4 >= 2:
        print('     trend by quartile (total / entry / exit, ms per window):')
        for i in range(4):
            s = series[i * q4:(i + 1) * q4] if i < 3 else series[3 * q4:]
            a = sum(x[0] for x in s) / len(s)
            e = sum(x[1] for x in s) / len(s)
            v = sum(x[2] for x in s) / len(s)
            print('        Q%d   %7.1f  %7.1f  %7.1f' % (i + 1, a, e, v))


def pass2(path):
    rows = []
    for line in open(path, errors='ignore'):
        for m in CENSUS.finditer(line):
            g = m.groups()
            ms = float(g[1])
            cells, zc = int(g[2]), int(g[8])
            fills, adm, dfr = int(g[6]), int(g[11]), int(g[12])
            if ms <= 0.0 or cells == 0:
                continue
            rows.append(([cells, zc, fills, adm, dfr], ms))
    if not rows:
        print('  PASS 2  no `Voxel recompute entry census` lines -- the leg ran without')
        print('          -VoxelRecomputeCensus, so the per-operation split is unavailable.')
        print('          (That is the "switch was off" reading, not "the costs are zero".)')
        return
    k = len(TERMS)
    A = [[0.0] * k for _ in range(k)]
    b = [0.0] * k
    for x, y in rows:
        for i in range(k):
            b[i] += x[i] * y
            for j in range(k):
                A[i][j] += x[i] * x[j]
    coef = solve(A, b)
    print('  PASS 2  per-operation fit over %d (level, window) observations' % len(rows))
    if coef is None:
        print('     ** singular: the counters are perfectly collinear on this leg and the')
        print('        split is NOT identifiable. Do not read shares. **')
        return
    total = sum(y for _, y in rows)
    pred = sum(sum(c * v for c, v in zip(coef, x)) for x, _ in rows)
    sse = sum((y - sum(c * v for c, v in zip(coef, x))) ** 2 for x, y in rows)
    rms = (sse / len(rows)) ** 0.5
    bad = False
    for (name, what), c in zip(TERMS, coef):
        share = 0.0
        for x, _ in rows:
            share += c * x[TERMS.index((name, what))]
        flag = ''
        if c < 0.0:
            flag = '   ** NEGATIVE: not identifiable from this leg **'
            bad = True
        print('     %-9s %9.1f ns/op   %9.0f ms  %5.1f%%%s'
              % (name, c * 1e6, share, 100.0 * share / total if total else 0.0, flag))
        print('               %s' % what)
    resid = total - pred
    print('     entryMs actual=%.0f  predicted=%.0f  RESIDUAL=%.0f (%.1f%%)  rmsErr=%.2f ms'
          % (total, pred, resid, 100.0 * resid / total if total else 0.0, rms))
    if bad or abs(resid) > 0.10 * total:
        print('     ** the model does not explain entryMs. The shares above are NOT a')
        print('        breakdown -- the fit is reporting that it failed. **')


def main(paths):
    for p in paths:
        print('== %s' % p)
        pass1(p)
        pass2(p)
        print('')
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
