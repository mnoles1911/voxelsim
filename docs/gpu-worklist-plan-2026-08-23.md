# P3: persistent worklist + indirect dispatch (plan, 2026-08-23)

This is the document `VoxelGpuWorklist.h`, `VoxelGpuWorklist.cpp` and
`VoxelWorklist.ush` cite. It records the arithmetic that justifies P3, what is
landed and verified, the exact A/B leg to run, and the kernel-conversion
sequencing that finishes the job.

## Why P3 exists, by arithmetic rather than hope

Measured (Saved/g3-stack.log, 2026-08-23): **15.0 passes per chunk** on the
per-chunk path, **14.0 per fused stack** (~4 chunks/stack achieved, ~8
assumed). Recorded hitch cliff: **~500 passes/tick** (= 30,000 passes/s at
60 fps).

| chunks/s | per-chunk passes/s | vs cliff | fused stacks | vs cliff |
|---------:|-------------------:|---------:|-------------:|---------:|
| 2,108 (today) | 31,620 | **1.1x** | ~7,400 | 0.25x |
| 50,000 (goal) | 750,000 | **25x** | 173,312 | **5.8x** |

Indirect dispatch: **~14 passes per TICK regardless of N** = 840 passes/s at
60 fps — **36x under the cliff at any chunk rate**. Batching is a constant
factor against a term that scales with N; indirect dispatch deletes the
scaling. The gate for P3 is therefore that **passes-per-tick goes flat as
chunk rate rises** — never throughput. The four-arm sweep (2026-08-23) cut
pass count 4.3x and moved throughput 0% at ~2,100 chunks/s: today's limiter
is admission, and every arm of this work is expected to leave throughput flat
until it isn't the limiter any more.

Sequenced AFTER the raster atlas (A), because the atlas empties the request of
its 46 KB raster window — without A there is no 64-byte record to put in a
ring. A is shipped and measured (served=1696, inlineFallback=0, gpuMiss=0).

## What is landed (and what each piece proves)

1. **The contract** (previous commit): `FVoxelGpuChunkWorkRecord` (64 B,
   static_asserted both sides), the host ring with monotonic cursors, and the
   args-setup kernel (`VoxelWorklistArgs.usf`) that turns "N records pending"
   into one DispatchIndirect triple per stage — pass count per tick constant
   by construction.
2. **The spine, wired and live** (this commit): behind `-VoxelGpuWorklist=1`,
   `FVoxelGpuMeshJobManager::DispatchBatch` appends a real record for every
   lean-eligible brick chunk (brick-only + no band + GPU pool shell + atlas
   request), `Tick` flushes once per tick, and the flush graph dispatches the
   **indirect spine prover** (`VoxelWorklistConsume.usf`) whose group count
   comes off the GPU-owned cursor. A 16-byte proof readback every ~5 s
   compares GPU consumption against the host mirror — consumed count, record
   fold (XOR of a 16-term per-record hash, mirrored C++/HLSL), and tail
   cursor, all three exact or the leg is declared invalid in the log. Proofs
   are only taken with nonzero consumption, so a passing proof can never be
   the vacuous `0 == 0`.
3. **Instrumentation**: the `[gpu-worklist]` window line leads with
   passes-per-tick (mean/max) and the chunk rate beside it — the pair to read
   first on any leg — plus ring counters, the appended==consumed+pending
   identity, per-reason skip counts, and proof tallies. `tools/leg-summary.sh`
   surfaces it; `tools/voxel-check-worklist-shader.ps1` compiles both kernels
   offline (~2 s, no editor).

**Not landed, said plainly: the generation kernels are unconverted.** Every
chunk still pays its classic passes; this arm's pass count is classic + a
constant 2/tick. The spine exists so the conversion lands stage by stage on a
ring whose transport is already proven under real traffic.

## The A/B leg

Matched legs, same flight, same seed, both with the current standard arm
(RetireQuads, `-VoxelGpuLeanBrickJobs=1`, `voxel.GPU.PoolAlloc`,
`-VoxelGpuRasterAtlas=1`):

* **Arm A (control)**: no worklist switch. Must be byte-identical to today —
  no `[gpu-worklist]` lines at all (their presence on a control leg is itself
  a failure).
* **Arm B**: `+ -VoxelGpuWorklist=1`.

Read arm B with `tools/leg-summary.sh`:

* **Records flowing**: `appended` ≈ the leg's brick-chunk dispatch count;
  `skips` explain any gap by name (noPack / quads / band / noAlloc /
  noAtlas). appended=0 with chunks flowing = the spine carries no traffic.
* **Proof**: `landed > 0`, `failed = 0`. Any FAIL invalidates the leg.
  landed=0 with consumption means GPU consumption is unverified — treat the
  spine as dead, not as quiet.
* **Identity ok**, `refusedFull` not growing (back-pressure; raise
  `-VoxelGpuWorklistCap` / `-VoxelGpuWorklistBudget` if it does).
* **Throughput and frame times: expect NO CHANGE.** The spine adds 2
  passes/tick and one 16 B readback per 5 s. A flat chunks/s number is the
  predicted result, not a failure — the pass term is not binding at 2,100
  chunks/s (1.1x cliff is intermittent hitch territory, visible in p95, not
  throughput).

**What would make us say it is NOT working**: any proof FAIL; identity DRIFT;
records=0 while eligible chunks dispatch (skips all zero too — the counters
themselves broken); proofs never landing despite consumption; `[gpu-worklist]`
lines on the control arm; or arm B moving throughput/p50 materially in either
direction (the spine is supposed to be ~free — if it costs, that is a finding).

## Stage 1 LANDED: the Column kernel (2026-08-23, -VoxelGpuWorklistColumns)

The first converted generation kernel, committed authored-not-yet-built.
`ColumnWorklistMain` (ue-project/Shaders/VoxelWorklistColumn.usf) is dispatched
ONCE PER TICK through the Column-stage indirect triple (16 groups per consumed
record) and computes every consumed record's 1,024 columns into a flush-level
column arena (budget x 1,024 GpuColumnSample; 20 MiB at the default 1,024
budget, logged at creation). The chunk's classic VoxelizeMain reads its slice
through `ColumnReadBase` (worldgen.ush) and the chunk's own ColumnMain pass is
SKIPPED. The math is not copied: worldgen.ush's ColumnMain body was factored
into `columnSampleAt` and BOTH entry points call the same text.

* Switches: `-VoxelGpuWorklistColumns=1` (on top of `-VoxelGpuWorklist=1`);
  byte gate `-VoxelGpuWorklistVerifyCols=1` (classic ColumnMain re-run per
  converted chunk + a compare pass into stats [4..5], riding the proof
  readback; +2 passes/chunk, verify arm only).
* Ordering: DispatchBatch appends, then FLUSHES (upload + args + column
  dispatch + prover), then enqueues the batch graph -- render commands execute
  in enqueue order, so the arena is written before VoxelizeMain reads it.
  With the stage off the flush stays in Tick, exactly where the spine was
  measured.
* Expected arithmetic: lean-alloc chunks drop 17 -> 16 passes; the spine's
  constant goes 2 -> 3/tick. passes/tick DROPS by ~chunks/tick but does NOT
  flatten -- six stages remain per-chunk. Throughput is expected NOT to move
  (admission is the limiter).
* Readings: `[gpu-worklist] wlcols conv= fb= arenaMissing= colverify checked=
  mism=`. FAILING: conv=0 with fb growing (stage converts nothing);
  arenaMissing>0 (flush/batch ordering broke -- every one fell back);
  mism>0 (converted columns differ from classic: LEG INVALID); checked=0 with
  conv>0 under the verify switch (the byte gate is dead).
* Exclusions, counted as fb: stack-fused members (AddBrickStackPasses takes no
  feed yet), records deferred past the budget, refused-full records.
* Offline gate: tools/voxel-check-worklist-shader.ps1 now compiles all 10
  kernels (both new entries, the factored classic forms in both atlas
  permutations, and the bench form) in ~2 s without an editor.

## The passes/tick counter: dead reading FIXED (2026-08-23, night 2)

The leg read `passes/tick mean=0.0 max=0` on both measured arms while the raw
mid-leg windows read 73-128. Three stacked faults, all in the instrument:

1. The window line's quiet gate compared the CUMULATIVE skip total to zero, so
   once any chunk had ever skipped, every post-flight linger window printed
   zeros forever -- and `tail -1` (the summary's read) landed on a linger
   line. The gate now compares the window's skip delta; the last printed line
   is the last ACTIVE window.
2. The spine's per-tick constant was tallied only inside DispatchBatch, so
   batchless ticks tallied 0 while dispatching 2-4 real passes. Tick now owns
   the constant and folds DispatchBatch's share in.
3. Nothing tripwired the tally. Every armed tick must now tally at least the
   2/tick args+prover floor, and a printed window below it appends
   `PASS TALLY DEAD` to its own line.

FAILING READINGS of the fixed counter: the `PASS TALLY DEAD` marker; any
printed window with mean below 2.0; `mean=0.0` without the marker (the
tripwire itself broken). Recovered from the night's logs with the fixed
filter: spine arm 116.8 passes/tick at 576 chunks/s = 17.6/chunk; cols+VERIFY
arm 128.3 at 598 chunks/s = 18.6/chunk -- exactly the verify arm's +1. The
17 -> 16 claim needs a NON-verify cols leg.

## Stage 2 LANDED: the Voxelize kernel (2026-08-23, -VoxelGpuWorklistVoxelize)

Authored-not-yet-built. `VoxelizeWorklistMain` (ue-project/Shaders/
VoxelWorklistVoxelize.usf) is dispatched once per tick through the
Voxelize-stage triple -- **16 groups per record, NOT the plan's provisional
512**: it keeps the classic one-thread-per-COLUMN mapping because the cave and
cavern reductions are per-column, so per-cell threads would recompute each 32
times and change the cost shape without changing a byte. It reads the column
arena (same flush graph, RDG-ordered) and writes a flush-level CELL ARENA
(128 KiB/record); the chunk's BrickClassify/BrickPack read their slice through
brickpack.ush's `CellReadBase` (classic dispatches compile a +0). The chunk's
own VoxelizeMain pass is SKIPPED: 16 -> 15 passes on the lean-alloc shape,
spine constant 3 -> 4/tick. worldgen.ush's VoxelizeMain body is factored into
`voxelizeColumnInto` and BOTH entry points call the same text.

* Switches: `-VoxelGpuWorklistVoxelize=1` (requires `-VoxelGpuWorklistColumns=1`
  -- the kernel reads the column arena; armed without it, everything falls
  back, counted); byte gate `-VoxelGpuWorklistVerifyVox=1` (classic
  VoxelizeMain re-run into the transient reading the SAME arena columns + a
  512-group compare into stats [6..7], riding the proof readback; +2
  passes/chunk, verify arm only).
* `-VoxelGpuWorklistCellBudget=<n>` (default 256): per-flush consume cap while
  armed -- the cell arena is 128 KiB/record, so the ring's default 1,024
  budget would be a 128 MiB arena; 256 is 32 MiB and 15,360 chunks/s of
  consume headroom. Sustained `pending>0` on the window line = raise it.
* ASSET CHUNKS FALL BACK BY DESIGN (`fbAssets`): AssetStamp writes cells
  between Voxelize and the brick chain, and stamping into the shared arena
  would put UAV barriers between every other chunk's reads of it. They keep
  classic Voxelize + AssetStamp until the flush-level asset buffer lands
  (step 5). The kernel's hasAssets early-out is group-uniform.
* Readings: `[gpu-worklist] wlvox conv= fb= fbAssets= arenaMissing= voxverify
  checked= mism=`. FAILING: conv=0 with fb growing (converting nothing);
  conv=0 with fbAssets growing and fb quiet (every chunk on this flight
  carries assets -- the stage is buying nothing, a flight fact to report);
  arenaMissing>0 (flush/batch ordering broke); mism>0 (LEG INVALID);
  checked=0 with conv>0 under the verify switch (dead gate).
* The pinned digest is untouched by construction: classic dispatches compile
  `CellReadBase = 0` / `cellBase + 0`, and the factored entry points call the
  same text the shipped kernels ran.

## Conversion sequencing (the P3 work proper)

Stage by stage, each stage gated before the next:

1. **Shared scratch arena**: per-slice Columns/Cells sized
   `budget x per-record` (128 KiB/record of cells; 1,024-record budget = 128
   MiB worst case — start with a smaller conversion budget, e.g. 256), indexed
   by record slot. This replaces per-region transient buffers and is the real
   re-plumb.
2. **Column** (16 groups/record): `VXC_WORKLIST` permutation of ColumnMain
   reading `Records[Control[0] + gid/16]`; atlas permutation required (the
   record carries no raster). Gate: byte-compare columns vs the classic
   dispatch of the same chunks, behind a verify flag.
3. **Voxelize** (512 groups/record), same pattern, gate on cells.
4. **ClassifyTotals / PackClaim / Write / Record**: the brick chain fused per
   the stage table (64 bricks/record allows an in-group scan — this REPLACES
   the region-wide 3+3 scan passes). Claim reads the record's ChunkSlot; Write
   dispatches worst-case groups and exits on the claim's actual counts. Gate:
   `voxel.GPU.VerifyBrickStack`-style byte compare of the landed pool state.
5. **AssetStamp** (order-preserving per-column gather, 16 groups/record): a
   NEW kernel, not a permutation — per-column loop over the record's
   `[AssetBase, AssetBase+AssetCount)` slice of a flush-level asset buffer
   that does not exist yet. Records already carry truthful AssetCount with
   AssetBase=0, so the buffer can be sized from live traffic. Until it lands,
   converted-chain eligibility excludes hasAssets records (bit 8, counted).
6. Each converted kernel `static_assert`s its groups-per-record against the
   host table (`kGroupsPerRecord`) — a mismatch is a torn dispatch.

Delivery on the converted chain is the lean-alloc shape: no readback, chunk
resident on the GPU's timeline, deliverable the tick after flush; claim
failures surface on the existing `[brick-gpualloc]` counters.

Re-measure before believing any of it helps (the architecture doc's own
rule): the conversion becomes NECESSARY somewhere above ~17,000 chunks/s
(500 passes/tick / 14 per stack x 8 chunks x 60 ticks); below that it is
pure overhead and the spine's own cost must stay invisible.
