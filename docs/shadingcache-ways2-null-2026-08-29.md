# ShadingCacheWays=2: the model was right and the mechanism was absent

2026-08-29. One armed leg (WAYS2-armed-a, -VoxelApplyColumnCacheWays=2).
Engagement exact: ways=2 sets=131072 slots=262144 on the line, hitWay2
1,652-2,321/window (hits a direct-mapped table cannot have).

    evict/miss:  43% (control family) -> 9-11%   [predicted ~11% -- HIT]
    cacheEvict:  ~370-490/window -> 76-100
    cacheMiss:   749-1,130 vs control 780-1,168  [FLAT -- the gate fails]
    frames:      hitches 2, maxMs 37.5, p95 10.80, p99 15.10, stutter 0.32
                 (in family or best-of-day; one leg, not credited to the arm)

The Poisson load-factor model predicted the eviction collapse to the digit
-- and the misses did not follow, which refutes the premise the arm was
built on: collision-evicted entries were NOT being re-demanded. The cold
population is COMPULSORY -- first touches of footprints the admission front
just delivered (~400/s in flight, L0/L1/L2 = 55/25/12 per CBL-map).
Default stays 1 (the arm costs +5 MiB and buys no misses); the code stays
as the honest instrument that settled this.

## The cold-shading campaign, closed end to end

One day, five arms/instruments, one chain of refutations, each naming the
next question:

    cold-burst census      -> bursts are 85-97/tick, coverable
    ColdShadingCapPerTick  -> REFUTED on holes (defer-time coverage volatile)
                              but proved stutters are not reqHdr bursts
    AtlasCoveragePadChunks -> SHIPPED (that was the atlas's own disc bug)
    WarmShadingAsync       -> mechanism flawless, candidate set disjoint
    coldByLevel            -> colds are L0-heavy, evictions suspected
    ShadingCacheWays=2     -> evictions collapse, misses flat: COMPULSORY

The queue-ahead warmer (sample pending-queue footprints on workers -- exact
coverage by construction) is REFUSED without being built: flight runs
admission-limited with the queue near-empty (exitEmpty=238/241, throughput
ladder), so there is no window between admission and submit for a worker to
fill. The only levers left for the 22 ms cold-burst tick are:
  (a) a cheaper sample (587 us = 4 amplifier columns + climate; fewer
      columns is a QUALITY question, i.e. product/owner territory), or
  (b) accepting it: after today the burst's visible cost is hitches 2-7 per
      120 s leg and maxMs ~37-46, against a 0.10% stutter goal whose
      remaining distance is diffuse (+2.1 tick / +3.2 render across 20
      frames in 9,000).

Nobody should build arm number six against this population without new
evidence.
