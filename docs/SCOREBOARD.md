# voxelsim performance scoreboard

**One page. Every number here is read from a named log line by a named tool.**
If a figure cannot be traced to a line below, it does not belong on this page.

Updated 2026-08-24. Baseline column = what the OWNER experiences (stock editor,
no extra flags). "Armed" = our leg config, which he does not run.

> ## THE TWO COLUMNS BELOW SWAPPED MEANING AT c52b2d2 — READ THIS FIRST
>
> **The owner decided to arm the GPU-primary set as shipping defaults**, so as
> of that commit the "armed" column IS what a stock editor with no extra flags
> runs. The "baseline (stock)" column is now HISTORICAL — it describes a
> configuration reachable only by passing `-VoxelGpuPrimary=0` explicitly.
> **Numbers in the table below have NOT yet been re-measured on the new
> default**; the re-measurement is the next leg. Do not quote the baseline
> column as "what the owner experiences" until this note is removed.
>
> **One flag, eleven behaviours.** `-VoxelGpuPrimary` is not one switch: it
> implies the GPU mesh fork ON, and re-points `GpuMeshInFlight` 256 -> 1024,
> `DispatchAheadCap` 0 -> 4096, `MeshBatchCap` **4 -> 64**, `MeshHarvestCap`
> 8 -> 0, and `LeanBrickJobs` OFF -> ON. The four sibling flags
> (`PoolAlloc`, `WorldGenBatch`, `StackClaim`, `RasterAtlas`) are armed
> alongside it. **Any sweep of one of those constants must now start from the
> primary-implied value, not the cvar default** — `MeshBatchCap`'s stock is
> **64**, not 4. This is the "three caps in series make a throughput number
> unattributable" hazard at a scale of eleven, and it is the price of shipping
> the block the measurements were taken on.
>
> **Atlas fill mode 3 (async) is NOT part of the set and stays OFF.** Arming
> the atlas master switch must never imply mode 3; if it ever does, that is a
> defect, not a tuning decision.
>
> **UNRESOLVED, AND IT OUTRANKS THE HEADLINE.** The matched leg behind the
> armed column (recorded in the body of `VoxelGpuPrimaryEnabled()`) reports
> `holes` **0 -> 10** and `R0` **6.4 s -> 16.4 s** under arming. Those are the
> two OWNER-VISIBLE DEFECTS below. The owner accepted this trade having been
> told the set *fixes* the arcs; on that leg it worsens them and de-prioritises
> the near ring he named first. Settling this is the first job of the next leg,
> and its failing readings are written into the code both ways so the leg can
> come out either way.

---

## 2026-08-24: RE-MEASURED ON THE SHIPPING DEFAULT, AND THE LANE REDIRECTS

Matched legs, one verified binary (`Result: Succeeded` + a second run reporting
"Target is up to date"), baked pose `-61440,-61440`, 1600x900, Flight=line,
`voxel.Stream.CoverageVerify` armed. The default now includes nearest-first
admission (see `VoxelStreamAdmission::NearestAdmitEnabled`).

| leg | cold | chunks/s | dispatched | R0 | p50 | p95 | stutter |
|---|---|---|---|---|---|---|---|
| drain 0 (default) | 22.3 s | 7,397/s | 1,108,557 | 12.2 s | 21.10 ms | 37.00 | 53.31% |
| drain 2 | 21.3 s | 7,725/s | 1,109,087 | 7.3 s | 21.20 ms | 39.00 | 53.77% |
| drain 4 | 21.1 s | 7,822/s | 1,109,054 | 6.5 s | 21.30 ms | 40.00 | 54.34% |
| drain 8 | 20.7 s | 7,967/s | 1,109,036 | 6.3 s | 21.00 ms | 40.00 | 53.91% |

**THE ADMISSION QUEUE IS NOT THE THROUGHPUT BOUND, and this is the second
failing reading of `AdmissionCapDrainSec()` firing exactly as written.** The cap
climbed 16x (2,048 -> 32,768) and dispatched work moved 0.05%. The +7.7% on
chunks/s is not extra work: total jobs is fixed by the world (~164,750), so
chunks/s = jobs / settle-time and it rose because settling got shorter. Per that
comment's own instruction the lane is redirected rather than re-run.

**WHERE THE BOUND ACTUALLY IS** (leg `I-bound3`, `-VoxelFramePhase=3`,
residualPct=9 so the reading is valid, no negative buckets):

    gameBusyMs=5.54  renderBusyMs=20.62  frameMs=22.91  floorFps=48
    tickMs=3.41 (15%)  gameWaitMs=15.22 (66%)  renderMs=20.62 (90%)  rhiMs=8.48
    bound=RENDERBOUND

The game thread is IDLE 66% of every moving frame, waiting on the render thread,
and the entire streaming tick is 3.41 ms inside a 20.62 ms render stage. The
streaming tick is 1.27% of WALL on the tick-budget line, with recompute at
0.0 ms. **Goal 2 and Goal 3 are one problem and it lives on the render thread.**
Deleting 100% of remaining game-thread streaming work moves 22.91 ms -> 20.62 ms:
48 fps -> 48 fps. Further admission-side throughput work cannot pay.

**TWO SCOREBOARD CLAIMS ABOVE ARE NOW SETTLED AND BOTH WERE OVERSTATED.**

*holes 0 -> 10 is VOID.* The GPU-primary flip states its own failing readings;
one of them is that holes=0 with `CoverageVerify` ARMED means the 0->10 was an
unarmed-watcher artefact. Every leg above reads `holesLast=0/30154` with that
watcher armed. Independently, segmenting the D-pair by regime (fill / flight /
parked) gives flight peaks of 8,382 stock vs 1,859 armed -- armed has 4.5x FEWER
holes while flying, the regime the owner reports. The `0 -> 10` came from the
parked linger window, which reads ~0 by construction.

*The steadiness cost is real but ~1/3 smaller than reported.* The sibling
failing reading predicted >=65% confirms and <31.4% refutes. Measured: 53-54%.
Neither; the cost reproduces at about two thirds of the table's figure.

**R0, THE DEFECT THE OWNER NAMED FIRST, IS LARGELY RECOVERED:** 15.8 s -> 6.3 s
across nearest-first (15.8 -> 12.7) and the drain cap (12.2 -> 6.3), against a
pre-arming stock of 5.5 s -- with moving p50 unchanged at 21 ms throughout.

---

## 2026-08-25: GOAL 3b RESTATED BY THE OWNER, AND THE CASCADE DEEPENED

**GOAL 3b IS NO LONGER "<= 0.10% STUTTERS". THAT NUMBER WAS NEVER THE OWNER'S.**
An agent chose it as its own reading of the word "steady", wrote it here, and it
was then quoted back to him as his requirement. He replaced it on 2026-08-25 with

    1% low (p99 frame time) >= 50 fps while MOVING at >= 20 m/s

which is what commercial practice actually reports (1% low / 0.1% low alongside
the average), and which cannot be gamed by a threshold count. **A stutter
percentage is a fragile gate**: `stutterPct` counts frames over a fixed 20.00 ms
bar, so a 20.1 ms frame scores identically to a 293 ms one. Keep reading it as
texture, never as the gate.

Shipped default today, 8,658 moving frames:

    mean 13.95 | p50 11.80 (85 fps) | p95 27.10 (37) | p99 34.00 (29) | max 293.04
    stutters >20ms   17.07%  = one every 0.07 s
    hitches  >33.3ms  0.92%  = one every 1.3 s

So the honest reading is 85 fps average with a **29 fps 1% low** -- the average
is nearly at target and the tail is at 34% of it, where a smooth-feeling game
wants roughly 50%. **The 293 ms maximum is a separate defect** from general
judder: a third of a second of freeze has a specific cause and should be hunted
as its own bug, not averaged into a percentage.

**THE CASCADE WAS DEEPENED** -- 7 rings at R0 = 64 m, same 4096 m range, 43%
fewer chunk iterations, ~3.4x fewer resident chunks, shipped as CODE DEFAULTS and
verified on a leg passing no ring arguments. See VoxelWorldSubsystem.h's
kDefaultRingPresets for the measured pair and the visual trade the owner
accepted. Goal 1 and the near-ring defect moved with it.

**ONE ARGUMENT IN THAT COMMIT IS WEAKER THAN ITS SENTENCE.** Commit 7e69459
supports the visual case partly with "hole RATE is unchanged (926/30168 = 3.1%
vs 263/8814 = 3.0%)". Flagged by its own author on 2026-08-25 and recorded here
rather than left standing: when fallthrough-off was later proposed, `uncovered`
moved **0.02 percentage points** and the owner rejected it on sight -- small
black arc holes at LOD boundaries. So that family of counters can barely move
at all for a defect he refuses instantly, and "hole rate unchanged, therefore
safe" is close to no information.

THE CASCADE CHANGE ITSELF STANDS. It was decided on a pinned-pose A/B that the
owner judged "very similar", and that is the evidence that carried it -- not the
counter. The distinction matters because the counter is the part a later reader
would be tempted to reuse. See [[test-must-be-able-to-fail]] shape: a
confirmation that cannot come out the other way is not a confirmation.

---

## THE THREE GOALS

| # | metric | baseline (stock) | armed | TARGET | status |
|---|---|---|---|---|---|
| 1 | cold start to settle | **45.5 s** | 18.1 s | **a few seconds (<= 5 s)** | FAIL |
| 2 | streaming throughput | **3,618/s** | 9,125/s | **50,000/s** | FAIL |
| 3a | frame p95 while MOVING >= 20 m/s | **44.00 ms (23 fps)** | 37.00 ms (27 fps) | **< 10.00 ms (>100 fps)** | FAIL |
| 3b | steadiness while MOVING | **31.4% stutters** | 51-58% | **<= 0.10% stutters** | FAIL |

**Goal 3 has two parts and both must pass.** A stutter is a frame over 20 ms —
a dropped frame at 100 fps. `hitches` (>= 33.3 ms) is the legacy 30-fps bar and
is kept only for comparability with historical legs; **it is not the gate.**

## OWNER-VISIBLE DEFECTS (judged by eye, not by counter)

| defect | state | resolution |
|---|---|---|
| black arcs / holes at LOD boundaries flying forward | PRESENT | capture pair, stock vs armed; owner judges |
| chunks load left-to-right, not toward facing | PRESENT | `NearestAdmit` + `ViewBias` are OFF by default |

---

## WHERE EVERY NUMBER COMES FROM

| metric | log line | how to read it |
|---|---|---|
| cold start | `Voxel cold settle: SETTLED t=..s` | `tools/leg-summary.sh`. **Read `peak:`, never `last:`** |
| throughput | same line, `mean=../s` | ditto |
| per-ring settle | same line, `R0..R5 t=..s` | R0 is the ring the player stands in |
| frame p95 / stutters | `SETTLED-MOVING scope=total ... p95Ms= stutterPct=` | **`SETTLED-MOVING` only.** Parked and fill are not the gate |
| the gate verdict | `gate=GOAL3-PASS/FAIL` | the AND of `gateP95` + `gateSteady` + `gateSpeed` |
| did the leg test the gate | `gateSpeed=` / `meanSpeedMps=` | below 20 m/s the leg is INVALID, not passing |
| thread ownership | `PIPELINE ... bound=` | void if `residualPct > 25` |
| game vs render | `PIPELINE gameBusyMs= renderBusyMs=` | the longer stage sets `floorFps` |
| holes | `voxel.Stream.CoverageVerify` | **defaults 0** — armed by harness only |

## THE CEILING THAT DECIDES GOAL 3

    seg                   gameBusy   renderBusy   floorFps   bound
    SETTLED-PARKED           1.69       9.23        108      RENDERBOUND
    SETTLED-MOVING          12.20      18.60         54      RENDERBOUND
    MOVING, after the
    admission fix            6.54      20.05         50      RENDERBOUND

**The render thread is the longer stage at every speed, parked and moving.**
Deleting 100% of remaining game-thread work cannot reach 100 fps. Goal 3 is a
rendering problem. **Parked is already 9.23 ms — a 108 fps ceiling with zero
terrain streaming.**

## WHERE I WANT TO END

    cold start   45.5 s  ->  <= 5 s
    throughput   3,618/s ->  50,000/s
    p95 moving   44 ms   ->  < 10 ms      (both at >= 20 m/s)
    stutters     31.4%   ->  <= 0.10%
    render frame 18.6 ms ->  < 10 ms      (this is the binding one)
    arcs         present ->  absent, owner-judged
    load order   L-to-R  ->  nearest-first, owner-judged

**Every arm reports its `SETTLED-MOVING gate=` alongside its headline. An arm
that improves cold start or throughput and worsens the moving p95 is not a win.**

## STANDING READING RULES (each earned by a retracted finding)
- Read `peak:`, never `last:` — the linger window reads zeros. **Ten wrong
  findings tonight came from the wrong window.**
- A percentage without its absolute is not a measurement.
- Wait time spread across N threads is not wall time.
- Game-thread milliseconds are not wall milliseconds (~0.25x on this pipeline).
- A bucket that cannot go negative is not a measurement.
- A residual that cannot stay large is not a residual.
- A check that has never failed is not yet known to be a check. **Six mutation
  arms exist in this codebase and none has ever been run.**

---

## 2026-08-25 (later): THE WORKLIST CLAIM CHAIN SHIPS, AND A CORRECTNESS BUG WAS FOUND UNDER IT

### Headline, matched legs (line flight 23.5 m/s, sun frozen 12:00 03-20)

    config                moving p99      stutters   hitches   doubleGrant
    stock (classic)       34.00 ms 29fps   17.08%       100        113
    worklist, cap 16      21.30 ms 47fps    1.2-3.3%     35          0

n=3 on the worklist arm (21.20 / 21.40 / 20.60). **The worklist chain now defaults
ON** (owner decision) -- all eight gates in `VoxelGpuMeshJobManager.cpp` latch to 1
when absent; `-VoxelGpuWorklist=0` is the control.

**The mechanism, not just the number.** `[gpu-worklist]` reads `passes/tick mean=25.1
max=76` at 4,260 chunks/s, replacing ~1,300 RDG passes built in a single render
command. The pass term went flat in N, which is what the chain was built to do.
Correctness on THIS build: `-VoxelGpuWorklistVerifyClaim=1` cross-checked
**207,983,035 dwords** against the classic path, `mism=0 dupRefused=0 fedNoBit=0`.

### The cap sweep closed, with a knee

    MeshBatchCap    64      32      16      8
    moving p99   35.0    22.0    21.3   22.0   ms

cap=16 is the minimum; cap=8 costs throughput without buying tail.

### THE POOL DOUBLE GRANT -- found, root-caused, fixed

At the SHIPPING `MeshBatchCap=64` the GPU pool allocator granted dwords somebody
already held: `doubleGrant == badFree`, 100-176 per leg, `UE_LOG(Error)` firing 9-13
times. Zero at cap<=16. It was NOT the worklist -- a stock leg with no worklist flags
shows it too. Reproduced on 4/4 cap=64 legs going back to RR-heavy, so it has been
live for days.

**Cause.** `FreeResident` appended `ChunkSlot` with no dedupe; `AllocateForChunk`
frees-then-reallocs on a same-key re-request and the coalesced first-fit arena hands
the SAME slot back. One slot entered a single free dispatch twice, and
`BrickPoolFreeMain` (`numthreads(64)`) plain-read `AllocSide` with no atomic claim --
both threads cleared the same bitmap range (one `badFree`) and both pushed the range
onto the free stack (one `doubleGrant` when popped twice). The exact equality of the
two counters is structural, not coincidence.

**Fix.** (1) `FlushPendingGpuFrees` dedupes the drained list and counts collapses into
a new `dupFreeSlots` on the `[brick-gpualloc]` line. (2) `BrickPoolFreeMain` takes the
slot with `InterlockedExchange` on the class words, so a duplicate sees class 0 and
frees nothing; base words are zeroed only on the branch that won its class.

**Confirmed on the exact leg that failed before**, and the gate could have failed:

    leg                 doubleGrant  badFree  Errors  dupSlot  dupFreeSlots
    UU-wl64  (before)       152        152      13       34        --
    VV-wl64fix (after)        0          0       0       24        19

`dupSlot`/`dupFreeSlots` non-zero prove the TRIGGER STILL FIRED and only the damage is
gone. `dupFreeSlots 0` on a cap=64 leg is NOT a pass.

**Standing rule:** `voxel.GPU.PoolAlloc`'s own comment says `doubleGrant>0` invalidates
any leg run on that default. Check `grep -c 'DOUBLE GRANT'` before quoting ANY leg.

### EVERY FRAME-RATE NUMBER ON THIS PAGE IS AT ~1552x873, NOT 2560x1440

`-ResX`/`-ResY` are INERT. A leg requesting 2560x1440 and a leg requesting 800x450
both rendered `view=1552x873`, byte-identical to every 1600x900 leg on disk. The
harness banner printed the requested size because it echoed its own argument.

The banner is fixed -- it now reads the marcher's `view=` and refuses a clean 'ok' on a
mismatch. **Reaching 2560x1440 is still unsolved** (ini overrides move the size but not
to target; `r.setres` voided the leg). The comparisons above are like-for-like so the
DELTAS stand, but the absolute fps figures are not at the resolution the target is
stated in. **Open question for the owner: what size is your PIE window?** If it is also
~1600x900 these numbers already describe what you see.

---

## 2026-08-26: GOAL 3b REACHED AT THE LINE -- p99 49.8 fps, from 29

Three matched flag-free legs, shipping defaults, no command line beyond the phase
instrument. Internal 1552x873, TSR upscaled to 2560x1440 (the owner's own config).

    leg          p50     p95     p99             holesFlight
    ZZ-final     9.40   15.70   20.20 (50 fps)      224
    ZZ-finalb    9.30   15.70   19.70 (51 fps)      213
    ZZ-finalc    9.40   15.90   20.30 (49 fps)      218

**Mean p99 20.07 ms = 49.8 fps against a >=50 fps goal.** Two legs clear it, one does
not. Reached WITHIN NOISE, not comfortably met -- a harder pose or a slower machine
misses. This morning: 34.00 ms / 29 fps.

Hitches 33 totalling 2624 ms with 1 >=200 ms, against 4795 ms / 35 / 10 this morning.

### What produced it -- NONE of it from the marcher

    worklist claim chain default ON    p99 34.00 -> 21.3   (208M dwords verified, mism=0)
    raster page rescue, cap 256        >=200ms freezes 10 -> 1, hitch time -47%
    speculative park default ON        redundant regeneration 13.6% -> 0.5%
    pool double-grant fixed            152/leg -> 0 at the SHIPPING cap

Verified engaged flag-free: stackSuppressed=31461, capHit=0, noAtlas=0, doubleGrant=0,
poolReplaced 0.5%.

### The marcher is untouched and is now the whole remaining problem

It is **54% of the GPU frame** and FIVE approaches were built and refuted:

    Z slab                  skipped 0.00% at the horizon
    4^3 block occupancy     23.7 tests/ray to avoid 11.2 cells, +30%
    beam pre-pass           refuted BY PROOF -- nothing in the index is provably empty
    sky mark + ladder       -7.6% WAS the terrain it deleted; honest version +3.1%
    RingCount reduction     -1.356 ms but pays in draw distance

**The rules those establish.** Nothing that adds a PER-STEP TEST can pay inside this ring
cascade (segments are 20-40 cells; there is never a long enough empty run to amortise
one). Nothing that infers emptiness from RESIDENCY can work (air is not resident, so
'empty' and 'not streamed' are the same bit pattern). And nothing may skip the retry
ladder on level-L evidence -- a coarse voxel is a downsample and can be solid over a
proven-air fine footprint, which is what a near mountain is MADE OF.

**Still open, in order:** adaptive per-ray ring depth (the one uncontested 1.356 ms);
rasteriser-bounded rays (GigaVoxels/SparseLeap/Teardown/Dreams all converged on it);
`kAnySolidBit` is a hardcoded 1 so the marcher's advertised cheapest skip has never fired.

---

## 2026-08-26 (later): THE MARCHER, MEASURED PROPERLY

Everything below is from matched legs on ONE binary, `view=1552x873` read from the
marcher's own line, `DOUBLE GRANT: 0` on every leg quoted.

### The law that governs this problem

**Marcher cost is RAY-COUNT LINEAR and nothing else moves it.** The 2026-08-25
screen-percentage sweep: 4.37 / 3.99 / 3.91 ms per Mray across a 5.7x range,
linear within 2%. Every per-ray knob is spent -- StepBudget flat across a 4x cut,
SkipLevels 1 and 2 byte-identical, AO free, Velocity free, ReachM inert
(`VoxelMarchRenderer.cpp:687`).

**That is why five empty-space arms died.** They were all attempts to make a ray
cheaper. The per-ray cost does not move. The lever is RAY COUNT.

### Half-res + jitter: four matched MOVING legs (`line`, 23.4 m/s)

    arm                  rays        p50    p95    p99      fps   lattice proof
    A control (ship)     1.355 Mray  9.20  15.40  19.70     51    full-res, correct
    B half-static        0.339 Mray  7.00  12.10  16.80     60    phasesSeen=1/1
    C half + jitter x4   0.339 Mray  7.00  11.90  16.70     60    phasesSeen=4/4 mask=0x0F
    D half + Halton x8   0.339 Mray  7.00  12.00  16.50     61    phasesSeen=8/8 mask=0xFF

**The jitter is FREE.** B/C/D spread (16.50-16.80) is narrower than the control's
own run-to-run spread. Half-res also lifts streaming throughput 26% (399 -> 504
chunks/s). Control reproduces the published band (19.70 vs 20.20/19.70/20.30).

**NO ARM READ INERT.** The jitter ran for the first time since it was written:
four and eight DISTINCT offsets logged, zero STATIC-lattice warnings.

### The jitter FAILED its own pre-registered falsifier

Frozen camera, frozen sun, settled world. Noise floor 0.0124% (control vs control):

    arm            frame-to-frame     first vs last (1.25 s)
    control        0.0004-0.0015%     0.0013%
    half-static    0.0001-0.0015%     0.0010%
    half-jitter4   0.337-0.416%       1.289%
    half-jitter8   0.301-0.436%       1.294%

Both unjittered arms converge to a still image. **Neither jittered arm settles** --
flat at ~0.35-0.42% with NO DECAY over 1.25 s, ~300x the control, diverging further
on a longer baseline. Concentrated in the **NEAR field** (bottom band 1.05%, top
band 0.07%) -- not the far field the arm exists to fix.

The arm's own falsifier named this in advance: "any new shimmer or crawl IN MOTION
refutes it even if the parked image improves."

**A MOVING CAPTURE DOES NOT EXIST AS A CAPABILITY HERE.** `voxel-capture.ps1` is
frozen-pose only; bolting a flight onto it produced a CONTROL that differed from
ITSELF by 70-81% frame to frame. That set was discarded, not reported. Judging
shimmer-in-motion needs the owner flying it or a video path nobody has built.

Captures for the owner's verdict: `Saved/jitter-captures-2026-08-26/`.

### The anySolid census: the skip that has never fired

`VoxelBrickTraverse.ush:2233` reads `VOXEL_MARCH_INDEX_ANYSOLID_BIT` and early-outs
-- "the cheapest possible skip". All three writers hardcode the bit to 1
(`VoxelMarchChunkIndex.cpp:867`, `:1362`, `VoxelMarchIndexScatter.usf:184`), so the
branch has NEVER EXECUTED. Such chunks are rejected one step later at
`LevelAndFlags & 0x10u` (`:2305`) -- AFTER a scattered 16-byte fetch into the
15.7 MB chunk table.

Census over 7 outcome buckets, identity PASS on all three poses:

    pose      probes/ray   pay for fetch   OF THOSE, WASTED   wasted/frame
    down          5.2         88.16%          35.11%             2.20 M
    horizon      85.5         28.79%          69.13%            23.06 M
    sky         194.4         12.10%          73.10%            23.30 M

The absolute wasted-fetch count goes 2.2 -> 23.1 -> 23.3 M, a 10x jump tracking the
1.108 / 4.45 / 5.638 ms cost curve.

**TEMPER THE EXPECTATION.** This box is an RX 7800 XT: 4 MB L2, 64 MB Infinity
Cache. The 15.7 MB table does NOT fit L2 but DOES fit MALL, so these are L2 misses
hitting Infinity Cache, not DRAM. And neighbouring rays in a wave probe the SAME
chunks -- nothing here measures distinct lines. **Expect single-digit to
low-double-digit percent of march time (~0.2-0.4 ms), NOT anything proportional
to 73%.**

Instrument costs +28% of march time (1.432/5.695/7.155 vs 1.108/4.448/5.638), so
those legs must never be quoted for ms. Left in place, defaulted off, rides
`voxel.March.HoleStats 1`.

### RETIRED: the open-sky mark as a tStart bound

**Structurally dead, with proof.** `MarkOpenSkyColumn` has ONE call site, inside
`EnumerateSurfaceFootprintCandidates`, reached only for footprints that passed
`EntryFootprintXYWanted`. It marks a RIND above the admitted top --
`min(SkyMarkChunks=8, SkyMarkMetres=512 m)`, i.e. **25.6 m at level 0**. The cap is
not a tuning oversight: the toroidal grid is 128 cells deep per level, so "mark the
sky" is NOT EXPRESSIBLE in it.

    marked cells      50,784 of 16,777,216   = 0.303%
    licensed blocks      125 of 262,144      = 0.048%
    not-proven-empty                         >= 99.09%

And the dilation fix that made the mark SOUND cost 4.4x of its licensing power
(mark's contribution to ladder skips 42.89 pp -> 9.70 pp), moving "unprovable gap
above admitted top" from 4.59% to **85.87% of columns**. The marked slab now FLOATS,
separated from the resident shell by unmarked blocking air, in 85% of columns.
A tStart collapses to that gap. **Do not revisit.**

### The elevation atlas is NOT a ceiling source

`VoxelRasterAtlas.cpp:842` is ONE `Tiles.elevationMm(px,py)` per pixel -- a POINT
SAMPLE, no max, no mean. At the shipped 1,875 mm pitch what it returns is a **cubic
B-spline CONTROL POINT, not the surface** (`voxelcore/tiles.h:51-56`). Terrain only,
no crowns. A sound ceiling is constructible from it but goes **silently unsound on a
coarse-tier run** (30 m pitch, raw samples, 2.98x prefilter sharpening gain --
`amplifier.cpp:1920-1930` calls that "a hole in the world").

Worth keeping: **absence is OUT-OF-BAND here**, uniquely in this codebase.
`rasterElevationMm` returns `kSeaLevelMm` on a miss, but `atlasResolve` returns a
BOOL off a per-slot page tag (`worldgen.ush:1473-1502`), so a consumer CAN tell
"no data" from "low ground".

### ZCut: the refutation was of its INPUT, not its structure

`voxel.March.ZCut` was recorded as "+2% at the horizon, -6% at sky, skipped 0.00%".
**The shader is sound and already clamps BOTH ends of every segment**
(`VoxelBrickTraverse.ush:4296-4475`) -- the near-end bound assumed missing is
already there. Three real defects, none inherent to the idea:

1. Its source `GetResidentChunkZBound` is a **cumulative union of resident chunk Z
   that never narrows** -- its own cvar help admits it "widens until the cut removes
   nothing". That is RESIDENCY, which cannot prove emptiness in this world.
2. The pad is 2^L -- **819 m at level 6** (`VoxelMarchRenderer.cpp:551-553`) plus a
   204.8 m in-shader bias. It structurally cannot bind at the coarse rings where a
   sky ray spends its time.
3. `bZCutRemoved` sets BOTH conjuncts of the retry gate (`:4453`, `:4459` vs
   `:4638`), so a merely-NARROWED segment force-buys a second full walk.

A sound world-derived bound already exists and runs every streaming tick:
`FootprintSurfaceUpperBoundMm` (`VoxelWorldSubsystem.cpp:14066`) ->
`Amplifier::surfaceUpperBoundMm`. Crown-inclusive at EVERY level, memoised,
declines rather than guessing. Swapping the source makes the soundness claim
STRONGER: a cut chunk goes from "provably not resident" to "provably all air".

**The go/no-go number is `H - Zc`**, relief above the camera within reach. World's
highest land is 6,331 m so no constant serves. If median headroom is kilometres
this is a null.

### Corrections to standing notes

- **Tallest baked asset is 45.0 m** (sitka spruce / western red cedar), tightened
  from the 60 m authored cap by `assetmanifest.cpp:442-462`. The "~17 m" figure in
  circulation is STALE. A flagged +7.4 m nominal-vs-baked discrepancy exists.
- `TExitUU` appears **0 times** in `VoxelBrickTraverse.ush`. `TMaxUU`'s real readers
  are `:4269` and a **second at `:4860`**.
- **Do NOT shrink the camera-centred box asymmetrically.** `bValid` is a dead test
  today, so an asymmetric box gives it a false population written out as a miss with
  ZERO instrumentation.
