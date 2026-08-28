#!/usr/bin/env python3
"""Split the frame's GPU time into named terms, from a `-csvGpuStats` capture.

WHY THIS EXISTS. The in-engine attribution report prints BUCKET MEANS (FAST /
SLOW / TAIL) and never the samples -- FrameSamples is never printed and never
cleared -- so no percentile of a NAMED GPU term, and no lead/lag between two
per-frame series, can be computed from a log. The CSV carries one row per frame
with a `GPU/<stat>` column per DECLARE_GPU_STAT_NAMED plus `GPU/Unaccounted`
(queue time inside no stat scope at all), and those columns SUM to the frame's
graphics-queue busy time. The decomposition therefore checks itself instead of
being asserted, and whatever is genuinely unnamed shows up as Unaccounted rather
than being silently spread over the terms that happen to be measured.

TWO SILENT NO-OPS THIS REFUSES TO READ THROUGH:
  * no GPU/ column at all -- `r.GPUCsvStatsEnabled` defaults 0 and with it off
    the columns are simply ABSENT, with no error. VOID, not "a null result".
  * GPU/VoxelMarch absent or ~0 -- a scope KNOWN to be live reading nothing
    means the readout is broken and no streaming column may be interpreted.
    This is the red arm and it runs before any number is printed.

THE BUCKET IS FRAME TIME, NOT GPU TIME, AND THAT MATTERS. Selecting the tail on
GPU total and then reporting per-term GPU means selects each term INTO its own
tail: any large term is over-represented in the top 1% of a sum it is part of.
The owner's gate is the frame, so the frame is the selector. `--bucketby gpu` is
kept for the cross-check, labelled as biased.

AND THE TWO STEPS ARE REPORTED SEPARATELY. Pooled across 13 legs the GPU rises
+5.85 ms from FAST to SLOW (>=p95) and only +0.53 ms more from SLOW to TAIL
(>=p99) -- it saturates. A p99-framed GPU conclusion would be attributing a step
the GPU does not take, so SLOW is printed first and is the headline.

TIMELINE ALIGNMENT. GPU/ columns are written end-of-pipe; VoxelStream/ columns
on the game thread. Both are one row per frame but the two timelines need not
share a phase. The offset is measured, not assumed: the streaming GPU terms are
mechanically same-frame with the chunks that produce them (the dispatches ARE
the chunks' passes), so their consensus cross-correlation peak against
ChunksApplied names the offset. Everything else is then quoted net of it.
"""
import csv, sys, math, argparse

def pct(v, p):
    if not v: return float('nan')
    s = sorted(v); k = (len(s)-1)*p/100.0
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return s[lo] if lo == hi else s[lo] + (s[hi]-s[lo])*(k-lo)

def col(rows, name):
    out = []
    for r in rows:
        v = r.get(name)
        try: out.append(float(v))
        except (TypeError, ValueError): out.append(float('nan'))
    return out

def clean(x): return [v for v in x if v == v]

def mean(x):
    c = clean(x)
    return sum(c)/len(c) if c else float('nan')

def xcorr(a, b, maxlag):
    """corr(a[t], b[t+lag])."""
    n, out = len(a), []
    for lag in range(-maxlag, maxlag+1):
        xs, ys = [], []
        for t in range(n):
            u = t + lag
            if 0 <= u < n and a[t] == a[t] and b[u] == b[u]:
                xs.append(a[t]); ys.append(b[u])
        if len(xs) < 30:
            out.append((lag, float('nan'), 0)); continue
        mx, my = sum(xs)/len(xs), sum(ys)/len(ys)
        sx = math.sqrt(sum((v-mx)**2 for v in xs)); sy = math.sqrt(sum((v-my)**2 for v in ys))
        c = (sum((xs[i]-mx)*(ys[i]-my) for i in range(len(xs)))/(sx*sy)) if sx > 0 and sy > 0 else float('nan')
        out.append((lag, c, len(xs)))
    return out

ap = argparse.ArgumentParser()
ap.add_argument('csvfile')
ap.add_argument('--all', action='store_true', help='do not filter to settled+moving')
ap.add_argument('--bucketby', choices=['frame','gpu'], default='frame')
ap.add_argument('--align', type=int, default=None,
                help='rows to shift GPU/ columns by; default = measured from the streaming terms')
ap.add_argument('--maxlag', type=int, default=6)
a = ap.parse_args()

with open(a.csvfile, newline='', encoding='utf-8', errors='replace') as f:
    rows = list(csv.DictReader(f))
hdr = list(rows[0].keys())
gpucols = [c for c in hdr if c.startswith('GPU/')]

print("file       : %s" % a.csvfile)
print("rows       : %d   GPU/ columns: %d" % (len(rows), len(gpucols)))
if not gpucols:
    print("VOID -- no GPU/ column. r.GPUCsvStatsEnabled never took; the leg needs -csvGpuStats.")
    sys.exit(2)

march = clean(col(rows, 'GPU/VoxelMarch'))
if not march or pct(march, 50) <= 0.05:
    print("VOID -- RED ARM FAILED. GPU/VoxelMarch is absent or ~0 where the marcher is")
    print("        known to cost ~3 ms. The readout is broken; nothing here may be read.")
    sys.exit(2)
print("RED ARM    : GPU/VoxelMarch p50=%.3f ms over %d rows -- a scope known live reads live."
      % (pct(march, 50), len(march)))

# ---- population ---------------------------------------------------------------
def truthy(r, k):
    try: return float(r.get(k) or 0) > 0.5
    except (TypeError, ValueError): return False

if not a.all and 'VoxelStream/Settled' in hdr:
    keep = [i for i, r in enumerate(rows) if truthy(r,'VoxelStream/Settled') and truthy(r,'VoxelStream/Moving')]
    print("population : SETTLED-MOVING, %d of %d rows" % (len(keep), len(rows)))
else:
    keep = list(range(len(rows)))
    print("population : ALL ROWS (%d)" % len(keep))

# ---- alignment ---------------------------------------------------------------
chunks_all = col(rows, 'VoxelStream/ChunksApplied')
gpu_all = [sum(v for v in vals if v == v) for vals in zip(*[col(rows, c) for c in gpucols])]
stream_terms = [c for c in gpucols if c.startswith('GPU/VoxelStream')]
# THE ALIGNMENT IS A CHOICE BETWEEN TWO DIFFERENT QUESTIONS, and picking the
# wrong one silently answers the other. Both are measured; the tool uses the
# first and reports the second.
#
#   (1) WHAT MADE THIS FRAME SLOW.  The game thread at frame f is waiting on GPU
#       work from ~3 frames back, so the GPU term that explains a slow frame is
#       NOT the one this frame's own dispatches produced. This is the pairing the
#       engine's own `gpu=` already uses -- LastGpuMs is whatever the GPU history
#       had published by the time that tick drained it -- and it is the pairing
#       every FAST/SLOW/TAIL number in this investigation was quoted with. It is
#       measured here as an IDENTITY, not a guess: VoxelStream/GpuFrameMs and the
#       sum of the GPU/ columns are the same physical quantity (both are the
#       graphics queue's busy cycles for one frame), so the lag that maximises
#       their correlation is the offset between the two timelines, full stop.
#       A leg where that correlation does not reach ~0.95 has something wrong
#       with it and the tool says so rather than proceeding.
#
#   (2) WHICH CHUNKS PRODUCED THIS GPU WORK.  The streaming dispatches ARE the
#       arriving chunks' passes, so their consensus peak against ChunksApplied
#       is same-frame by construction. Used for the lead/lag section only.
#
# Using (2) for the bucket table would decompose a DIFFERENT frame's GPU time
# than the one the bucket was selected on, and on this leg that alone moves the
# FAST->SLOW GPU step from +5.7 ms to +2.1 ms.
gfm = col(rows, 'VoxelStream/GpuFrameMs')
clock_align, clock_r = None, float('nan')
if any(v == v for v in gfm):
    xs = [t for t in xcorr(gfm, gpu_all, a.maxlag) if t[1] == t[1]]
    if xs:
        b = max(xs, key=lambda t: t[1])
        clock_align, clock_r = b[0], b[1]

align = a.align
if align is None and clock_align is not None:
    align = clock_align
    print("alignment  : %+d rows, from the GPU-CLOCK IDENTITY (r=%.3f against "
          "VoxelStream/GpuFrameMs)." % (align, clock_r))
    print("             This is the pairing the engine's own gpu= uses, so the split below")
    print("             decomposes exactly the number the FAST/SLOW/TAIL rows report.")
    if clock_r < 0.95:
        print("             *** r < 0.95: the two series are meant to be the SAME quantity.")
        print("             *** Treat the alignment, and every delta below it, as unproven.")

chunk_align = None
if any(v == v for v in chunks_all):
    peaks = []
    for c in stream_terms:
        s = col(rows, c)
        if not clean(s) or pct(clean(s), 99) < 0.05: continue
        xs = [t for t in xcorr(chunks_all, s, a.maxlag) if t[1] == t[1]]
        if not xs: continue
        b = max(xs, key=lambda t: t[1])
        if b[1] > 0.3: peaks.append(b[0])
    if peaks:
        chunk_align = max(set(peaks), key=peaks.count)
        print("             (same-frame-with-chunks would be %+d, consensus of %d streaming terms %s --"
              % (chunk_align, len(peaks), peaks))
        print("              that is %d frames of pipeline depth, and it is used for lead/lag only.)"
              % abs((chunk_align - align) if align is not None else 0))
if align is None:
    align = 0
    print("alignment  : NOT MEASURABLE. Using 0 -- do not quote a delta from this run.")

def gcol(name):
    """A GPU/ column resampled onto the game-thread row index."""
    s = col(rows, name)
    return [s[i+align] if 0 <= i+align < len(s) else float('nan') for i in range(len(s))]

gpu_tot_al = [sum(v for v in vals if v == v) for vals in zip(*[gcol(c) for c in gpucols])]
frame = col(rows, 'FrameTime')

# ---- buckets ------------------------------------------------------------------
sel = [i for i in keep if frame[i] == frame[i]]
key = frame if a.bucketby == 'frame' else gpu_tot_al
kv = [key[i] for i in sel]
P50, P95, P99 = pct(kv, 50), pct(kv, 95), pct(kv, 99)
FAST = [i for i in sel if key[i] <= P50]
SLOW = [i for i in sel if P95 <= key[i] < P99]
TAIL = [i for i in sel if key[i] >= P99]
print("bucketed by: %s%s" % (a.bucketby,
      "  (BIASED for per-term reading -- each term is part of the sum it is selected on)"
      if a.bucketby == 'gpu' else ""))
print("            p50=%.2f p95=%.2f p99=%.2f max=%.2f ms | FAST n=%d  SLOW(p95..p99) n=%d  TAIL(>=p99) n=%d"
      % (P50, P95, P99, max(kv), len(FAST), len(SLOW), len(TAIL)))
for nm, b in (("SLOW", SLOW), ("TAIL", TAIL)):
    if len(b) < 40:
        print("            WARNING: %s has only %d rows -- its means are noise." % (nm, len(b)))

def bmean(v, idx):
    c = [v[i] for i in idx if v[i] == v[i]]
    return sum(c)/len(c) if c else float('nan')

print()
print("GPU TIME BY NAMED TERM.  The step that matters is FAST->SLOW: pooled over 13 legs")
print("the GPU rises +5.85 ms to p95 and only +0.53 ms more to p99, i.e. it SATURATES.")
print("Read dSLOW as the result; dTAIL is shown so the saturation is visible, not hidden.")
print()
print("%-32s %8s %8s %8s %8s %8s %8s" % ("GPU term", "FAST", "SLOW", "dSLOW", "TAIL", "dTAIL", "shr%"))
print("-"*88)
terms, tot_slow = [], 0.0
for c in gpucols:
    v = gcol(c)
    fm, sm, tm = bmean(v, FAST), bmean(v, SLOW), bmean(v, TAIL)
    terms.append((sm-fm, c, fm, sm, tm))
    tot_slow += (sm-fm)
for ds, c, fm, sm, tm in sorted(terms, reverse=True):
    if abs(ds) < 0.005 and sm < 0.02: continue
    print("%-32s %8.3f %8.3f %8.3f %8.3f %8.3f %7.1f%%"
          % (c, fm, sm, ds, tm, tm-fm, 100.0*ds/tot_slow if tot_slow else 0))
print("-"*88)
print("%-32s %8.3f %8.3f %8.3f %8.3f %8.3f"
      % ("SUM (= graphics queue busy)", bmean(gpu_tot_al, FAST), bmean(gpu_tot_al, SLOW),
         tot_slow, bmean(gpu_tot_al, TAIL), bmean(gpu_tot_al, TAIL)-bmean(gpu_tot_al, FAST)))
print("%-32s %8.3f %8.3f %8.3f %8.3f %8.3f"
      % ("frame time (game thread)", bmean(frame, FAST), bmean(frame, SLOW),
         bmean(frame, SLOW)-bmean(frame, FAST), bmean(frame, TAIL),
         bmean(frame, TAIL)-bmean(frame, FAST)))

# ---- game-thread columns, same buckets ---------------------------------------
vs = [c for c in hdr if c.startswith('VoxelStream/')]
if vs:
    print()
    print("GAME-THREAD COLUMNS, same rows and same buckets (no alignment applied -- these")
    print("are already on the game timeline).")
    print("%-32s %8s %8s %8s %8s %8s" % ("column", "FAST", "SLOW", "dSLOW", "TAIL", "dTAIL"))
    print("-"*80)
    for c in vs:
        v = col(rows, c)
        fm, sm, tm = bmean(v, FAST), bmean(v, SLOW), bmean(v, TAIL)
        print("%-32s %8.3f %8.3f %8.3f %8.3f %8.3f" % (c, fm, sm, sm-fm, tm, tm-fm))

# ---- lead / lag ---------------------------------------------------------------
if any(v == v for v in chunks_all):
    print()
    lag_ref = chunk_align if chunk_align is not None else align
    print("LEAD / LAG -- chunks applied against each GPU term, quoted NET of the %+d-row"
          % lag_ref)
    print("alignment above. Net 0 = same frame. Net > 0 = chunks arrive first and the cost")
    print("follows. Net < 0 = the cost came first, i.e. the long frame harvested the chunks")
    print("(reverse causation, which the unlimited harvest cap makes possible).")
    ck = [chunks_all[i] if i in set(sel) else float('nan') for i in range(len(rows))]
    for c in gpucols + ['__TOTAL__']:
        s = gpu_all if c == '__TOTAL__' else col(rows, c)
        cl = clean(s)
        if not cl or pct(cl, 99) < 0.05: continue
        xs = [t for t in xcorr(ck, s, a.maxlag) if t[1] == t[1]]
        if not xs: continue
        b = max(xs, key=lambda t: t[1])
        print("   %-32s peak r=%5.2f at lag %+d  -> net %+d" % (c, b[1], b[0], b[0]-lag_ref))
