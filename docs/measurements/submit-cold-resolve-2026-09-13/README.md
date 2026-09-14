# The p99 hitch is a cold asset resolve, and three caps in a row were hiding it

Phase 2 of the slice plan. Nine walk captures at the temperate forest site,
`-154740,-81476`, 256 m ring, Nanite default, every receipt passed. The chain
below is in the order it was found, because each step's null is what named the
next one.

## 1. The hitch is `SubmitMs`, and inside it, assets

`voxel.Stream.FrameAttribution` had never been switched on at this site. Mode 2
collects nothing here — it selects on `VoxelFramePhase`'s settled flag, which
reads `settled=0` on all 36 windows of a walk capture — so mode 1 is the
instrument, with the caveat that its buckets pool cold fill.

The six-way submit split, previously log-only, is now a per-frame CSV stat.
Over the worst 1% of frames by game thread:

| column | median ms | max |
|---|---|---|
| SubmitMs | 281.104 | 420.398 |
| SubAssetsMs | **277.360** | 408.363 |
| SubReqHdrMs | 4.336 | 14.547 |
| SubMgrMs | 0.324 | 6.849 |
| **SubRasterMs** | **0.059** | 0.133 |

**Assets is 98.8% of the bracket and raster is 0.02%.** The standing hypothesis
— the synchronous raster-atlas page fill, from the 2026-08-25 flight
investigation — is refuted at this site. It was written down before the run.

## 2. Inside assets: the cache works, the misses are ruinous

    caller=GpuSubmit calls=93784 hits=86221 (91.9%) coldMisses=7563
                     inlineMs=197438  maxRawResolveMs=526.2

`inlineMs` accrues only on misses. So **86,221 cache hits cost 2 ms between
them**, and 7,563 misses cost 197 seconds of game thread across the capture. One
resolve peaked at **526 ms** — a single call, a visible freeze.

## 3. The misses are COARSE, and coarse could never be warmed

Cold misses were not split by level until now. Split:

| | misses | resolve ms | ms each | share of miss time |
|---|---|---|---|---|
| level 0 | 847 | 3,195 | 3.77 | 1.6% |
| **coarse** | **6,716** | **194,162** | **28.91** | **98.4%** |

A coarse footprint resolves over a rect 2^L wider per side, finds far more
instances, and costs 7.7x what a level-0 one does.

**Three caps in series then explain why the prewarm never touched them**, each
found by instrumenting the refusal rather than by reasoning:

1. **The queue only ever held level 0.** `WarmPredictiveAssetResolves` enqueued
   `Key{0,...}` and nothing else, so no coarse footprint was ever a candidate.
   *Refuted first:* raising all four caps with a level-0-only queue moved cold
   misses 7563 → 8247, i.e. not at all. The gate written beforehand was "the gate
   is coldMisses, not launched=", and it fired.
2. **The site bound refused them.** With coarse keys queued, `refusedUnbound`
   became 4,624,719 — `footprintResolveSitesBound` caps *speculative* warming at
   8,192 candidate sites, which coarse rects exceed. **The inline path has no
   such bound.** So for every rect between the two, the cap did not prevent the
   work; it guaranteed the work happened on the game thread. Ceiling raised to
   1<<20, and `refusedUnbound` went to **0**.
3. **The cache issued eight warm tokens.** `FootprintResolveCache`'s constructor
   defaults `pending=8`, and the UE side used the default constructor — so at
   most eight warms could be outstanding whatever `launchCap` and `inFlightCap`
   said. `refusedNoToken` was 4,906,442. This is the one that mattered.

## 4. What the fix is worth

Two runs per arm, pooled, defaults otherwise identical:

| arm | frame p50 | p95 | **p99** | max | >33 ms | >100 ms |
|---|---|---|---|---|---|---|
| control | 14.17 | 20.74 | **146.90** | 731.26 | 2.46% | 1.49% |
| raised + 512 MiB cache | 14.22 | 20.12 | 113.91 | 584.88 | 2.63% | 1.17% |
| **raised, no extra memory** | 14.20 | 20.15 | **109.92** | 741.36 | 2.33% | 1.10% |

**p99 falls 25% and it is outside run-to-run spread** — the control's two runs
read p99 129.86 and 153.00, the arm's 101.26 and 116.17, and the ranges do not
overlap. Cold misses fall 7,563 → ~3,400 and inline resolve time 197 s → 127 s.

**The extra 448 MiB of cache bought nothing** (113.91 against 109.92), so it is
not being spent: the two memory knobs keep their old defaults.

**Throughput did not move**, which is the trade this could have made and did
not: chunks applied per frame 10.15 → 10.00 and mean GPU 13.28 → 13.30 ms.

Verified with the defaults flipped and **no flags on the command line at all**:
`launchCap=64 inFlightCap=256 probeCap=1024 queueCap=16384`, zero `dpcvars`,
cold misses 3,396, hit rate 96.4%, p99 88.73, frames over 100 ms 0.88%.

## What is NOT fixed, and it is the part a player would still feel

**Frames over 300 ms did not improve** (0.31% control against 0.38% armed), and
the worst frame is still ~730 ms. Halving the cold misses halved the *moderate*
tail; the extreme tail is a different population — single enormous resolves, the
worst seen at 686 ms, demanded in the same frame they are needed, which no
amount of warming ahead can anticipate.

The shape of the remaining fix is deferral rather than prediction: when a
submit's resolve is cold and a coarser ancestor is resident and holds terrain,
defer the chunk instead of paying the resolve inline. That machinery already
exists in this file for cold *shading* — `ColdShadingCap`, with its
coverable/holeRisk split and its `capExempt` counter — and the hole-safety
argument transfers unchanged. That is the next piece of work, and it is a
bounded change to a path that already has the shape.
