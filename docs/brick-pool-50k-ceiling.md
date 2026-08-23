# Brick pool at 50,000 chunks/s: what it must sustain, and where it breaks

2026-08-23. Companion to the three pool commits of this date (claim-size
histogram + reconciliation instruments; free-stack cap at the provable bound;
wrap-safe counters). Numbers below come from the first passing P1 legs
(1,101,676 claims, `claimFail 0`, `xcheck 408 ok / 0 FAIL`) and from the pool's
own recorded means (~194 occ / ~208 mat dwords per claiming chunk). Where the
claim-size histogram will refine a number, that is said.

## The one-line answer

The pool's data structures are not the ceiling at 50,000 chunks/s. Its **arena
sizes** are — and they scale with the **resident set**, not with the rate. If
residency is ever meant to reach the full 262,144 slots, the occupancy arena
must grow **192 → 288 MiB**; the material arena is already big enough once its
class step is fixed, and that fix roughly **doubles its effective capacity**.
Everything rate-shaped either was fixed this date (free-stack leaks, counter
wrap) or belongs to the dispatch/submission architecture, not to the allocator.

## What 50,000 chunks/s actually asks of the pool

**Bytes.** Mean actual claim is 194 + 208 = 402 dwords = 1,608 B/chunk, so
50k/s writes ~80 MB/s into the arenas. VRAM bandwidth is hundreds of GB/s;
this is nothing. **A premise correction that matters:** padding does *not*
multiply write bandwidth — the copy kernels copy ACTUAL words (`claim[2]`,
`claim[4]`), never the padded range. Padding is purely an arena-*footprint*
tax. "18x the rate = 18x wasted bandwidth" is not the failure mode; "the same
resident set costs 1.6x the arena it should" is.

**Events.** 50k claims/s plus, once resident is at capacity, ~50k frees/s.
Per event the allocator does a handful of atomics — O(1), no scan, no
coalescing. The structures scale; the **dispatch shape does not**: today every
claim is its own 1-thread compute pass plus per-chunk copy/desc/record passes,
so 50k/s is 200–400k passes/s — far past render-thread submission, and pass
count is already a measured cost at 2.8k/s (hence B.1 fusion and the P3
worklist). The allocator is ready for the fix: the claim kernel is already
parameterized per fused-stack member, and the natural end state is **one claim
dispatch per batch** (N threads, N chunks). That change should ride whenever
generation itself batches; it is submission architecture, shared with the
admission and worklist owners, not a pool-private lever.

**Free-list traffic.** 50k slots/s × 4 B = 200 KB/s of upload. Nothing.

## Arena arithmetic (the real ceiling)

Footprint = resident claiming chunks × padded mean + stranded + leaked.
Leaked is now zero by construction (free-stack cap = ChunkCapacity); stranded
is printed per window.

Tonight's reading, cross-checked two ways: inFlight 131.9 MiB occ at 256
padded dwords/chunk ⇒ ~135k occ-claiming residents; 240.7 MiB mat at 512
padded ⇒ ~123k. Consistent: **~130k resident GPU-allocated chunks** of the
262,144 slot capacity.

**Occupancy (192 MiB arena).**
- At today's padding (256 padded dwords = 1,024 B/chunk): exhausts at
  **~196,600** claiming residents — *below* the 262,144 slot capacity.
- The step sweep barely helps here: ceil(194/64)·64 = ceil(194/128)·128 = 256,
  so OccStep 64 is an arithmetic no-op at the mean; step 32 gives 224 padded ⇒
  ~224,700 chunks — still short of slot capacity.
- **Recommendation: if full-slot residency is a goal, raise occ 192 → 288 MiB
  (1.5x) — the number to have now.** 262,144 × 1,024 B = 256 MiB of demand,
  plus stranding/churn margin. If residency stays at today's ~130–150k, the
  current 192 MiB holds with ~30% headroom and nothing need move.

**Material (448 MiB arena).**
- At step 512: 2,048 B/chunk ⇒ exhausts at **~229,400** residents — mat is the
  *first* arena to fail on the road to full slots today, and its ~59%-at-the-
  mean padding is the dominant term in the 39.7% combined figure.
- At step 256 (if the histogram confirms the ~208-dword cluster): 1,024 B/chunk
  ⇒ ~458,700 chunks — covers full slot capacity with 1.75x margin. **The mat
  step fix is worth ~230 MiB of effective capacity, more than any arena raise
  under discussion.** Do not shrink the mat arena on the back of it until the
  histogram's p99 tail has been read: the worst legal claim is 8,448 dwords and
  the tail's weight is exactly what the means cannot show.
- Candidate steps are on the log every window now (`claim sizes mat: ...
  padding by step { 128:… 192:… 256:… 384:… 512:… }`) — pick from one leg's
  reading, not from this document's projection.

**Rate enters the arenas only through churn transients** (claims land before
the frees of the chunks they displace): bounded by jobs in flight — thousands
of chunks, single-digit MiB. Negligible against the numbers above.

## What was rate-fragile and is now fixed

- **Free-stack overflow** (leakedRuns 16,736 / 1,164 / 29,012): cap now
  defaults to ChunkCapacity, which a short induction makes overflow-proof (a
  class's ranges are created only when its stack is empty, at which instant
  all of them are held by distinct resident slots). 25 MiB of state at default
  geometry. `leakedRuns 0` is now a theorem; the window line's
  `stackPeak occ/mat of <cap>` measures how deep bursts actually go, and a
  peak above cap (or any leakedRun) now indicts the invariant, not the tuning.
- **Counter wrap**: the padded/actual dword accumulators wrapped uint32 in
  ~112 s at 50k/s (~2.5 h at today's rate — soak legs were already exposed).
  Harvest now folds modular deltas into int64; worst 5 s window at 50k/s is
  22x under the modular limit.

## Open items, honestly

1. **Batched claim dispatch** — the pass-count wall above. Pool-side delta is
   small once generation batches; coordinate with admission/worklist.
2. **Verify rotor coverage**: 8 sampled chunks per 5 s window is a vanishing
   fraction of 50k/s. Not a correctness hole (the gate needs N > 0, and
   `0 ok / 0 FAIL` already reads as UNTESTED), but worth scaling with rate
   when the rate arrives.
3. **The 40-count shells/claims mismatch**: now attributable from the log —
   the window line prints `shells N (stolen S, unclaimed U)`. U is in-flight
   work plus one-window staleness while streaming; **U holding non-zero at
   quiescence after counters land is the failing reading** (shells whose
   claims never ran). The two prime suspects are rule-2 stolen shells (now
   counted separately) and CPU-producer packs dropped before flush.

## Reading the next leg

Throughput will probably NOT rise from any of this: the limiter is admission
(~2,805/s, separately owned). **Flat chunks/s with lower padding, leakedRuns 0,
and clean FAIL counters is the success reading, not a null result.** The gate:
`padding` falls (mat especially, per the histogram's chosen step), `leakedRuns
0`, `stackPeak ≤ cap`, `doubleGrant 0`, `badFree 0`, `xcheck N ok / 0 FAIL`
with N > 0, `unwritten 0` while `claimFail 0`, and `evictions` /
`writesDropped` flat.

## Addendum 2026-08-23 (later the same day): re-derived against the 99% leg

The four-arm eviction matrix (Saved/ev-A..D.log) landed a measurement this
document did not have: **the DEFAULT configuration runs the pool at 99.0% of
slot capacity** (arm A: residentAll 259,582/262,144, evictions 156), and the
same window's `[brick-gpualloc]` line read **occ 136.1/192.0 MiB (70.9%) and
mat 248.4/448.0 MiB (55.4%)**. Three corrections follow, and they moved the
shipped geometry.

**1. Slots bind first, not the occupancy arena.** The "192 MiB exhausts at
~196,600 claiming residents" projection above sized arenas from the per-CLAIM
mean (~194 occ dwords). But the histogram on the same leg shows **716,774 of
1,136,504 claims (63.1%) carry zero payload words** — uniform chunks store
collapsed, descriptors and a record only. The honest basis is amortized
per-SLOT cost, measured directly at 99% residency:

    occ:  136.1 MiB / 259,582 slots = 550 B/slot   (step-128 padding, 16.8%)
    mat:  248.4 MiB / 259,582 slots = 1,003 B/slot (step-512 padding, 50.9%)

At full 262,144-slot residency: occ ~137.5 MiB (72% of 192), mat ~251 MiB
(56% of 448). **Today's default needed no arena raise at all — it needed
slots.** The "192 → 288" recommendation was right by accident of direction
only; its arithmetic double-counted padding onto chunks that never claim.

**2. The shipped geometry (same-day commit).** Slots 262,144 → 393,216
(covers the scale-1.25 + band-4 wide ring at ~302,800 demand = 77%, and even
the mult-1.15 variant at ~343,300 = 87%; exhausts at ring scale ~1.44). Occ
192 → 288 MiB, because full-slot demand at the new count is 393,216 × 550 B
≈ 206 MiB — 192 would have refused before the slots did, inverting the
orderly-failure ordering (slot exhaustion evicts farthest-first; arena
exhaustion claim-fails and leaves unwritten volume). Mat stays 448 MiB.

**3. The mat step, chosen from the histogram as instructed.** Projections on
the leg: `{ 128:17.9% 192:25.5% 256:32.2% 384:42.4% 512:50.9% }`, p99 ≤ 896
dwords. **Shipped 512 → 256**, not 128, because free-stack state is
(classes × ChunkCapacity × 4 B): 66 mat classes at step 128 cost +50 MiB of
state at 393,216 chunks against ~47 MiB of incremental arena saving — a wash
carrying double the class-count stranding risk. At 256, measured-mix mat cost
falls 1,003 → ~726 B/slot (~110 MiB effective at full slots). The occ-64
claim in this doc is also corrected by the histogram: it is NOT an arithmetic
no-op (projection 7.6% vs 16.8% — the mass is not at the mean), but nets only
~6 MiB after stack growth, so occ stays 128.

**VRAM, before → after** (committed, GPU-alloc armed): desc 128 → 192, occ
192 → 288, mat 448 → 448, records 16 → 24, free-stack/alloc state ~29 → ~68
MiB. **Total ~813 → ~1,020 MiB** (+207 MiB), inside the approved 1–2 GB.
Control geometry on one binary: `-VoxelBrickPoolChunks=262144
-VoxelBrickPoolOccMiB=192 -VoxelBrickPoolMatMiB=448 -VoxelGpuPoolMatStep=512`.

Also fixed: `Voxel brick lifetime`'s occ/mat columns read the CPU arenas,
which a GPU-alloc leg never touches — they printed a structural 0.0% through
the entire eviction matrix. GetUsedOccWords/GetUsedMatWords now return the
landed GPU snapshot on an armed pool (one window stale, like the
[brick-gpualloc] line they mirror).
