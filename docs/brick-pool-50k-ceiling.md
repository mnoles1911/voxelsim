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
