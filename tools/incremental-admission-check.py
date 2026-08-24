#!/usr/bin/env python3
"""Decide a -VoxelIncrementalAdmission leg, unambiguously.

    python tools/incremental-admission-check.py Saved/on.log Saved/off.log

WHY A READER AND NOT A GLANCE. This switch has produced three separate wrong
readings already:

  * `incr=0` during cold fill is CORRECT -- a level's first scan has no
    predecessor to diff against -- and is indistinguishable from broken. Five
    windows of a running leg were once read as "zero traffic" when the
    complete run showed the fast path carrying most scans. This script
    therefore reports the WARM portion separately and refuses to give a
    verdict from the cold portion at all.
  * `p2-on.log` prints `forced=`/`deferred=`; the current build prints
    `first=`/`edit=`. The fields are NOT comparable -- the old build counted a
    refill rescan as a full-sweep cause, which is the exact defect
    bLevelRefillRescan was added to fix. This script detects the format and
    REFUSES the old one rather than silently diffing across it.
  * A percentage without its absolute is not a measurement. Every ratio below
    prints its numerator and denominator.

WHAT DECIDES IT, and both directions are stated:

  IDENTITY   sum(incr) + sum(full causes) must EQUAL sum(scans) from the
             `Voxel recompute (max ...)` line. The eligibility gate is an
             if/else-if chain, so a scan that took an uncounted path is
             impossible BY CONSTRUCTION -- which means a non-zero delta is not
             a rounding error, it is a code change that broke the chain, and
             nothing else in the leg may be trusted.
  TRAFFIC    warm incr% is the fired/not-fired reading. Near 0 warm = the fast
             path never fires and the leg measures the switch's OVERHEAD only.
             Near 100 = it fires.
  DELETION   footprints per hitch frame must FALL. This is the counter that
             separates "work deleted" from "work moved", and a timing cannot
             do it. See the caveat below -- it is hitch-gated today.
  COST       ms per scan, normalised by scans so a leg with a different
             recompute cadence still compares.

THE CAVEAT THAT MATTERS MOST. `ThisFrameLevelFootprints` is reset every frame
and printed ONLY on the hitch line, above the 33.3 ms threshold. So it is
sampled only on the WORST frames -- which are the ones most likely to be full
sweeps -- and its sample rate FALLS as the feature works. Every deletion ratio
here is therefore biased AGAINST the incremental arm and is a lower bound.
Hook I in docs/incremental-admission-audit-2026-08-23.md adds the window
accumulator that removes the bias; until it lands, read these as "at least".

AND THE RULE THIS PROJECT LEARNED TONIGHT: mechanical success is not
throughput. A fall in footprints and ms/scan is a mechanism working. What
decides whether it ships is COLD SETTLE on a matched pair, and this script
prints that too -- and says so when it disagrees with the mechanism.
"""

import re
import sys

INC = re.compile(r'Voxel incremental admission \(5s window\): incr (R0=.*?) \| full: '
                 r'(\w+)=(\d+) (\w+)=(\d+) (\w+)=(\d+) (\w+)=(\d+)')
MAXL = re.compile(r'Voxel recompute \(max since last log\):.*?\| scans (R0=\d+.*?) \| tracked')
SUML = re.compile(r'Voxel recompute \(sum since last log\): totalMs=([\d.]+) fineMs=([\d.]+) '
                  r'exitScanMs=([\d.]+) queueFilterMs=([\d.]+) sortMs=([\d.]+) \| entryMs (.*)')
HITCH = re.compile(r'Hitch frame recompute: recomputeMs=([\d.]+).*?footprints (R0=\d+.*?) \| tracked=')
# Cold settle is NOT in the log -- it is computed by tools/voxel-run-leg.ps1's
# own settle loop and printed to its stdout. It must therefore be PASSED IN
# (--settle a,b). A script that silently reported "settle unavailable" would
# quietly reduce every verdict to the mechanism, which is exactly the mistake
# the fine-tier lock fix made.
SETTLE = None
RN = re.compile(r'R(\d)=([\d.]+)')

NLEV = 7
COLD_WINDOWS = 12  # the fill's opening: every level's first scan lives here


class Leg:
    def __init__(self, path):
        self.path = path
        self.fmt = None
        self.incr = []          # per window, per level
        self.causes = []        # per window, dict
        self.scans = []         # per window, per level
        self.entry = []         # per window, per level ms
        self.total = []         # per window, recompute ms
        self.hitch_fp = [0] * NLEV
        self.hitch_n = 0
        self.settle = None
        self._load()

    def _load(self):
        for line in open(self.path, errors='ignore'):
            m = INC.search(line)
            if m:
                if self.fmt is None:
                    self.fmt = 'old' if m.group(2) == 'forced' else 'new'
                self.incr.append([int(v) for _, v in RN.findall(m.group(1))] + [0] * NLEV)
                self.causes.append({m.group(k): int(m.group(k + 1)) for k in (2, 4, 6, 8)})
                continue
            m = MAXL.search(line)
            if m:
                v = [0] * NLEV
                for l, x in RN.findall(m.group(1)):
                    v[int(l)] = int(float(x))
                self.scans.append(v)
                continue
            m = SUML.search(line)
            if m:
                v = [0.0] * NLEV
                for l, x in RN.findall(m.group(6)):
                    v[int(l)] = float(x)
                self.entry.append(v)
                self.total.append(float(m.group(1)))
                continue
            m = HITCH.search(line)
            if m:
                self.hitch_n += 1
                for l, x in RN.findall(m.group(2)):
                    self.hitch_fp[int(l)] += int(float(x))
                continue

    # --- aggregates ------------------------------------------------------
    def active(self):
        """Window indices where recompute actually ran."""
        return [i for i, t in enumerate(self.total) if t > 0.0]

    def sum_scans(self, lo=0):
        return sum(sum(self.scans[i]) for i in self.active() if i >= lo and i < len(self.scans))

    def sum_incr(self, lo=0):
        return sum(sum(self.incr[i][:NLEV]) for i in self.active() if i >= lo and i < len(self.incr))

    def sum_causes(self, lo=0):
        t = 0
        for i in self.active():
            if lo <= i < len(self.causes):
                t += sum(self.causes[i].values())
        return t

    def sum_entry(self, lo=0):
        return sum(sum(self.entry[i]) for i in self.active() if i >= lo and i < len(self.entry))


def report(leg):
    print('== %s' % leg.path)
    if leg.fmt is None:
        print('   NO `Voxel incremental admission` line: the leg ran WITHOUT the switch.')
        print('   That is the "switch off" reading, not "the fast path never fired" --')
        print('   a distinction with no shared symptom, which is why it is printed.')
        print('   Valid as the CONTROL arm; ms/scan and footprints below still compare.')
        fp = sum(leg.hitch_fp)
        print('   DELETION   footprints per hitch frame = %.0f  (%d over %d hitch frames)'
              % (fp / leg.hitch_n if leg.hitch_n else 0.0, fp, leg.hitch_n))
        print('              per level %s' % [round(x / leg.hitch_n) if leg.hitch_n else 0
                                              for x in leg.hitch_fp])
        e, sn = leg.sum_entry(), leg.sum_scans()
        print('   COST       entryMs=%.0f over %d scans = %.3f ms/scan'
              % (e, sn, e / sn if sn else 0.0))
        return True
    if leg.fmt == 'old':
        print('   ** OLD LOG FORMAT (forced=/deferred=). REFUSED. **')
        print('   That build counted a refill rescan as a full-sweep cause, which is the')
        print('   exact defect bLevelRefillRescan was added to fix. Its incr% is not')
        print('   comparable to the current build and must not be diffed against it.')
        return False

    n = len(leg.active())
    warm = [i for i in leg.active() if i >= COLD_WINDOWS]
    lo = COLD_WINDOWS

    si, sc, sn = leg.sum_incr(), leg.sum_causes(), leg.sum_scans()
    wi, wc, wn = leg.sum_incr(lo), leg.sum_causes(lo), leg.sum_scans(lo)

    print('   active windows=%d  (warm portion: %d, after the first %d)' % (n, len(warm), COLD_WINDOWS))
    print('   IDENTITY   incr(%d) + causes(%d) = %d   vs scans %d   delta=%d  %s'
          % (si, sc, si + sc, sn, si + sc - sn,
             'OK' if si + sc == sn else '** BROKEN: a scan took an uncounted path **'))
    print('   TRAFFIC    whole leg %d/%d incremental = %.1f%%' % (si, sn, 100.0 * si / sn if sn else 0.0))
    print('              WARM ONLY %d/%d incremental = %.1f%%   <- the fired/not-fired reading'
          % (wi, wn, 100.0 * wi / wn if wn else 0.0))
    if wn == 0:
        print('              ** no warm windows: the leg is all cold fill and CANNOT decide this **')
    elif wi == 0:
        print('              ** WARM incr = 0: the fast path never fires. This leg measures the')
        print('                 switch\'s backlog-insert OVERHEAD and nothing else. **')
    print('   causes     %s' % ', '.join('%s=%d' % kv for kv in sorted(
        {k: sum(c.get(k, 0) for c in leg.causes) for k in leg.causes[0]}.items())))
    fp = sum(leg.hitch_fp)
    print('   DELETION   footprints per hitch frame = %.0f  (%d over %d hitch frames)'
          % (fp / leg.hitch_n if leg.hitch_n else 0.0, fp, leg.hitch_n))
    print('              per level %s' % [round(x / leg.hitch_n) if leg.hitch_n else 0
                                          for x in leg.hitch_fp])
    print('              (hitch-gated and therefore biased AGAINST this arm: read as "at least")')
    e = leg.sum_entry()
    print('   COST       entryMs=%.0f over %d scans = %.3f ms/scan' % (e, sn, e / sn if sn else 0.0))
    if leg.settle is not None:
        print('   SETTLE     %.1f s   <- THE number that decides shipping' % leg.settle)
    else:
        print('   SETTLE     NOT SUPPLIED. Cold settle is not in the log -- it comes from')
        print('              tools/voxel-run-leg.ps1 settle loop. Pass --settle=a,b.')
        print('              Without it this leg can say the MECHANISM works and nothing more.')
    return True


def compare(a, b):
    print('\n== VERDICT  %s  vs  %s' % (a.path, b.path))
    sa, sb = a.sum_scans(), b.sum_scans()
    ea, eb = a.sum_entry(), b.sum_entry()
    ma = ea / sa if sa else 0.0
    mb = eb / sb if sb else 0.0
    fa = sum(a.hitch_fp) / a.hitch_n if a.hitch_n else 0.0
    fb = sum(b.hitch_fp) / b.hitch_n if b.hitch_n else 0.0
    print('   ms/scan            %.3f  vs  %.3f   (%.2fx)' % (ma, mb, mb / ma if ma else 0.0))
    print('   footprints/hitch   %.0f  vs  %.0f   (%.2fx, lower bound)' % (fa, fb, fb / fa if fa else 0.0))
    if a.settle is not None and b.settle is not None:
        print('   SETTLE             %.1f s  vs  %.1f s   (%+.1f%%)'
              % (a.settle, b.settle, 100.0 * (a.settle - b.settle) / b.settle))
        mech = mb / ma if ma else 1.0
        if mech > 1.10 and a.settle >= b.settle:
            print('   ** MECHANISM WORKED, THROUGHPUT DID NOT. ms/scan fell %.2fx and settle did'
                  % mech)
            print('      not improve. That is the fine-tier-lock shape: a cost removed from a')
            print('      thread that was not the binding one. Do not ship on the mechanism. **')
    else:
        print('   SETTLE             unavailable -- this pair can only say whether the')
        print('                      MECHANISM works, never whether it is worth shipping.')


def main(argv):
    paths = [a for a in argv if not a.startswith('--')]
    settles = None
    for a in argv:
        if a.startswith('--settle='):
            settles = [float(x) for x in a.split('=', 1)[1].split(',')]
    legs = [Leg(p) for p in paths]
    if settles:
        for l, v in zip(legs, settles):
            l.settle = v
    ok = [report(l) for l in legs]
    print('')
    if len(legs) == 2 and all(ok):
        compare(legs[0], legs[1])
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
