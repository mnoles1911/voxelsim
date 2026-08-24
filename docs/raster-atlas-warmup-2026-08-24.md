# The raster atlas warmup: where the 2.83 ms/page goes, and the ladder that removes it

Branch `worktree-agent-rasteratlas-warmup`. **Authored, not built, not run.**
Files touched: `ue-project/Source/VoxelEarth/VoxelRasterAtlas.{h,cpp}` only.
No hooks required in any other agent's file — every switch is command line.

---

## 1. THE MEASUREMENT, from a log that already existed

`Saved/q-L4ship.log`, cold start settles at **t = 22.7 s** (`Voxel cold fill:
t=22.7s jobs=164733 pending=0 inFlight=0`).

### 1a. The atlas page fill — the item I was given

    win  fills   ms GT     resident
      1    205   553.1      205/1681
      2    280   878.0      485/1681
      3    272   812.6      757/1681
      4    286  1225.6     1043/1681   <- PEAK window; I read this one for the rate
      5    478   834.5     1521/1681
      6+     0     0.0     1521/1681   steady state, gpuMiss=0, fills=0

    1,521 pages, 4,303.8 ms of game thread, 2.830 ms/page.
    4.30 s / 22.7 s = 18.9% of cold start.

**It is invisible because it is in no named bucket.** It is the unattributed
residual inside `tickMs`. Cross-check, window at `00.52.12` (`win=2.02s`):

    tickMs 1026.2 - (recompute 305.8 + dispatch 332.0 + apply 32.5 + 0.1) = 355.8 ms residual
    atlas fill 878.0 ms over its 5.0 s window, prorated to 2.02 s          = 355 ms

Those agree to 0.2%. A second window gives 399 vs 326 — the windows were 5 s on
one line and 2 s on the other, which is the defect fixed in §4.

### 1b. THE SECOND GAME-THREAD RASTER TERM, which nobody had counted

`Voxel gpu submit split`'s `raster=` bucket, same leg, cold start only:

    635.1 246.4 31.0 545.5 154.5 93.2 84.3 159.9 253.3 462.4 191.1  =  2,856.7 ms

then **0.1-0.8 ms/window for the rest of the run.** In the same span the atlas
served 114,169 requests and declined 2,354.

`raster` is **86% of the whole `dispatch` bucket** in the peak windows
(635.1 of 741.2; 545.5 of 634.4). The budget doc ranks `dispatch` third among
game-thread costs — during warmup it is almost entirely this.

### 1c. THE TOTAL, and why the two terms are one problem

    atlas page fill   4.30 s   18.9% of a 22.7 s cold start
    submit raster     2.86 s   12.6%
    -------------------------------------
    TOTAL             7.16 s   31.5%

They are coupled: the inline fill exists **only because the atlas has not warmed
yet**. Warming faster deletes the second term rather than moving it. That makes
the addressable item roughly twice what was handed to me.

**What is NOT yet measured, and the new counter that fixes it:** `raster`
brackets *two* things — `PrepareRequest`'s coverage walk (114,169 calls) and the
inline `FillRasterWindow` a decline falls back to (2,354 calls). If it were all
inline fill that is 1.21 ms per decline; if the walk is expensive it is much
less. The new `[raster-atlas] prepare:` line prints `prepareMs` so
`raster - prepareMs` is the inline half. **Do not quote 1.21 ms/decline until
that line has been read.**

---

## 2. WHERE THE 2.83 ms GOES — the instrument, not the answer

I do **not** yet know the split, and the first leg is what answers it. What
ships is the breakdown itself, on every window line, in every mode:

    [raster-atlas] fill: mode=... | ok | perPage 2.830 ms = elev X (n%) + climate Y (n%)
      + stage Z (n%) + resid R (n%) | callsPerPage elev=16384 climate=16384 of 16384 px
      (dedup off, 1x) | audit checked=0 mismatch=0 | async pages=0 launches=0 inFlight=0
      prime=0 | disc 1194/1521 pages | lifetime fills=N win=2.00s

* Three phases, one `Cycles64` pair each: **six timer reads per page against
  32,768 sampler calls, ~0.005%.** The phase split changes no value —
  `elevationMm` and `climate` are pure in (px,py).
* **The residual is printed**, in ms and as a share. A phase added or skipped
  without a bucket shows as a residual that grows, and >20% prints
  `SUSPECT RESIDUAL`.
* **Absolutes before shares**, every bucket. (The rule the lock-contention null
  bought: `waitShare=99%` of a denominator that never became wall time.)

### The two failing readings, which do not print the same

| reading | printed |
|---|---|
| pages filled, elevation phase made zero calls | `FAIL PHASE-NEVER-RAN: ... the instrument is outside the path, not a zero cost` |
| calls > 0 and a phase timer read zero cycles | `FAIL TIMER-DEAD: ... the timer is broken, the work is not free` |
| no pages filled at all | `IDLE (steady state, not a dead counter)` |

Three distinct states. The old line could print only one zero for all three.

### The lead I was handed, and how the instrument settles it
32,768 shared lock acquires per page at 0.026-0.264 us brackets 1-8 ms, which
contains 2.83. **That bracket is compatible with several splits and is not a
conclusion.** If elevation and climate come back at ~50/50 with ~85 ns/call
each, per-call overhead is the story. If elevation dominates 4:1, the block
lookup and spline evaluation are. The buckets decide it, not the bracket.

---

## 3. THE LADDER — `-VoxelGpuRasterAtlasFill=N`, default 0

| mode | what it adds | what it is |
|---|---|---|
| 0 | today | control; still prints the buckets |
| 1 | fill the coverage **disc**, not the Chebyshev **square** | fewer pages |
| 2 | + **climate dedup** | fewer calls per page |
| 3 | + **off the game thread** | **removes the term** |

### Mode 1 — the sweep fills a square, coverage is a disc
`RadiusPages` is a Chebyshev radius **rounded up** from `CoverageRadiusPx`, and
the sweep walks the whole 39x39 = 1,521-page square. Rings admit on a RADIUS
(`AdmitOuterUU`), so the corners — about 22% — are never requested. Predicted
1,521 -> ~1,194 pages.

Cannot generate wrong terrain even if the disc were too tight: a page the sweep
skips is still demand-queued and inline-filled by `PrepareRequest`. **The cost
of being wrong here is `inlineFallback`, not silence.**

### Mode 2 — climate is sampled 256x more often than it varies
The fine tier carries no climate plane. Every `climate()` lands in the COARSE
sampler through `FineTileSampler::climate`'s
`floorDiv(px * 1875, 30000) = floorDiv(px, 16)`. One coarse cell is
**16 x 16 = 256 atlas pixels**; a 128-px page needs **8 x 8 = 64** distinct
samples and mode 0 makes **16,384** calls.

`scale=1` on the tile-grid line is `tilePixelSizeMm(1) = 30000`, so the leg
passes `-VoxelGpuRasterAtlasClimatePitchMm=30000`.

**Why a switch and not derived here:** `ITileSampler` exposes exactly one pitch
(its own), and `FineTileSampler::climate`'s comment warns in as many words
against restating the conversion as a hardcoded `/16`. So this file is *told*,
and if nobody tells it the dedup does not run: the line prints
`climateDedup=UNAVAILABLE`, which is a different reading from `on, 1x`.

**The gate, and how to watch it go red.** 64 probes per page re-sample climate at
deterministic pseudo-random pixels and compare against what the dedup wrote.
A wrong run length disagrees on ~half the pixels of any page whose climate
varies. `-VoxelGpuRasterAtlasClimatePitchMm=60000` gives run 32 and **must**
produce `FAIL: climate dedup audit MISMATCH` on the first varied page, disable
the dedup for the session, and say the leg is invalid.

What it cannot catch, stated: a wrong run over genuinely uniform climate — where
the deduped value is also the right value. `checked` prints beside `mismatch` so
`checked=0` can never be read as `mismatch=0`.

### Mode 3 — the term removal, and the hazard it is written around
Sampling runs on `UE::Tasks` workers into slot buffers; the game thread only
harvests finished slots, memcpys them into the delta and flips the mirror.

**THE HAZARD, because it would have killed the leg.**
`FVoxelFineTileSamplerProxy::elevationMm`'s two cold paths are not symmetric:

* **game thread** — non-resident pixel blocks on a synchronous tile load. Slow,
  correct, and what the fill does today (`blockingLoads=0` on this leg).
* **worker** — the same pixel goes to `ReportGateLeak_Locked`, which is
  **FATAL on an unattended run**, by deliberate design.

Naively moving this fill to a worker converts a silent blocking load into a
killed leg. **The guard:** each page is primed on the game thread with four
corner samples before it is handed over. A page is 128 px and a fine tile is
8,192, so four corners name exactly the tiles the page can span; the game-thread
branch runs, and the worker only ever reads resident tiles. Four calls against
32,768 — 0.01%.

Residual risk, stated rather than hidden: `TickResidencyAndEviction` could evict
between prime and read. Nothing evicts on a cold start (1.82 of a 12.00 GiB
budget, `ringRadius=0`), but the reading is `gateLeaks>0` on the `Fine tier`
line, and **mode 3 must not ship on that evidence.**

**Ordering is unchanged.** A worker writes only into its own slot. Nothing is
staged and no tag flips until the game thread harvests a *finished* slot, in the
same tick the delta flushes, before any dispatch is enqueued. A page in flight
reads NOT resident, so `PrepareRequest` declines it and the chunk takes the
inline path — correct, just not yet cheap.

---

## 4. THE THIRD HARDCODED WINDOW, in a third file

`kWindowSeconds = 5.0` was hardcoded here while every flight leg passes
`-VoxelPerfLogInterval=2`. The old comment claimed the 5 s cadence made the atlas
line "land beside the lines it will be read against" — it did the opposite: this
line's windows were **2.5x wider** than `Voxel tick budget`'s, so the two
side-by-side residual cross-checks in §1a had to be prorated by hand.

Now read from the same switch, with real elapsed `win=%.2fs` appended at the END
of every line (old-leg-grep rule).

**A FOURTH, not fixed because the file is not mine:** `Voxel gpu submit split`
prints the literal label `(5s window)` on lines that are 2.0 s apart. Its values
are sums, not rates, so no arithmetic is wrong — but the label is, and someone
will divide by it. Owner of `VoxelWorldSubsystem.cpp`, that is a one-line fix.

---

## 5. THE REGISTERED DISPROOF — written before anything was measured

> Mode 3 must move `[raster-atlas] window: ... ms GT` to ~0, with `asyncPages > 0`
> proving the work still happened, **and** cut cold settle by roughly the
> game-thread time it removed. If the game-thread time vanishes and settle does
> not move by **at least half** of what was removed, the atlas fill was NOT on
> the critical path. Modes 1-3 are then tuning a term nobody waits for, and the
> answer is to **revert them and write down why**, not to tune them further.

Concretely, against the 22.7 s control: mode 3 removes ~4.3 s of game thread and
should also collapse most of the 2.86 s inline-fallback term. **Settle must fall
by >= 2.2 s** (half of 4.3) or the work is dropped.

The lock-contention null does not transfer, and the reason matters: that wait
was spread over ~36 worker threads and never became wall time. **This is game
thread, serial, ahead of the rings.** But that is an argument for *testing* it,
not for believing it — hence the number above.

### Mechanism confound, bounded in advance
Mode 3 changes both *where* the fill runs and *how fast pages arrive*. Those are
separable from the log: `inlineFallback` totalled 2,354 over the whole cold
start, and `raster - prepareMs` is what they cost. If settle improves by more
than that plus the removed `ms GT`, something else moved and the result needs a
second look.

---

## 6. COUNTERS, with the failing reading stated BOTH ways

| counter | healthy | "never fired" | "fired but wrong" |
|---|---|---|---|
| `elevCalls` / `climateCalls` per page | 16384 / 16384 (mode 0-1), 16384 / 64 (mode 2-3) | `FAIL PHASE-NEVER-RAN` in words | timer zero -> `FAIL TIMER-DEAD` |
| `resid` | small, printed in ms and % | — | `SUSPECT RESIDUAL > 20%` |
| `disc N/M` + `lifetime fills` | mode 0 fills -> M, mode>=1 -> N | the two totals do not separate = mode 1 did not arm | — |
| `dedup` | `on, 256x` | `UNAVAILABLE` (pitch never passed) | `on, 1x` (armed, saved nothing) |
| `audit checked` / `mismatch` | `checked>0 mismatch=0` | `checked=0` — the audit never ran | `mismatch>0` -> FAIL, dedup latched off, leg invalid |
| `async pages` / `launches` | `pages ~= launches x batch` | `pages=0` — mode 3 did not arm | `pages << launches x batch` = scheduler-starved |
| `ms GT` | mode 3: ~0 | — | mode 3 with `ms GT` still ~4 s = the fill did not move |
| `prepare calls` | > 0 while dispatching | `calls=0` = DEAD reading | — |
| `gpuMiss` | 0 | — | `>0` = wrong terrain shipped, leg invalid (pre-existing) |

---

## 7. THE LEG

One build, four arms. **One rung per arm** — the house rule.

    tools/voxel-run-flight-leg.ps1 ... -VoxelPerfLogInterval=2 -VoxelGpuRasterAtlas=1 \
        -VoxelGpuRasterAtlasFill=0                                         # atl-f0  CONTROL
        -VoxelGpuRasterAtlasFill=1                                         # atl-f1  disc
        -VoxelGpuRasterAtlasFill=2 -VoxelGpuRasterAtlasClimatePitchMm=30000 # atl-f2  + dedup
        -VoxelGpuRasterAtlasFill=3 -VoxelGpuRasterAtlasClimatePitchMm=30000 # atl-f3  + off-thread

Plus one **gate arm that must go RED**, and it is worth more than the four above:

    -VoxelGpuRasterAtlasFill=2 -VoxelGpuRasterAtlasClimatePitchMm=60000     # atl-red

must print `FAIL: climate dedup audit MISMATCH` within the first few windows.
**If it does not, the audit is not in the path and mode 2 must not ship** — a
check only ever observed passing is not a check.

**`atl-f0` alone answers task one.** If the breakdown says climate is a small
share, drop mode 2 and go straight to 1 and 3.

Read the **peak** window for any rate (`tools/leg-summary.sh` prints `peak:`);
`last:` is the drain tail. Every number in §1 above is from named windows, and
the peak window is called out where it matters.

---

## 8. WHAT I COULD NOT DO FROM THESE FILES — the biggest remaining lever

Elevation is **irreducible in call count**: all 16,384 values per page are
genuinely distinct. The only way to make it cheaper is a **cheaper way to ask**,
and `vxc::ITileSampler` is a per-pixel virtual interface with no bulk form.

A rect API on the fine sampler would resolve the tile and the decoded block
**once per page** instead of 16,384 times, and take the lock once instead of
16,384 times. Per pixel that replaces {1 RW-lock pair, 3 `unordered_map` finds,
~6 int64 divisions, 1 virtual call} with **one array index**. Owner of
`voxel-core` / `VoxelFineTileStreamer`: `FineTileSampler::prewarm` already
proves the block walk; the shape needed is

    bool elevationRect(int64 px0, int64 py0, uint32 w, uint32 h, int32* out);

Even at a conservative 10x this turns the elevation phase from ~2 ms/page into
~0.2 ms — which is a bigger win than everything in §3, and it makes mode 3
unnecessary rather than merely better. **It is not in my files, so it is a
recommendation with arithmetic, not a change.**
