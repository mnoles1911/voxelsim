# R0 entry profile, first enabled capture, 2026-09-10 (captures 28 ON / 29 OFF)

Binary: branch `claude/r0-entry-profile-rebase-2026-09-10` (PR #254 head + the rebased
R0 diagnostic), coherent `voxel-build.ps1 -Verify`. Same scene, cache and settings as the
size-cull A/B OFF arm (full 256 m ring, `temperate-authored-lods-full-2`). Both captures
carry passing receipts with identical module hashes; the ON manifest records
`-VoxelR0EntryProfile -VoxelRecomputeCensus`.

Engagement proof: the ON log has seven five-second `R0EntryProfile inclusive` lines and
frames.csv carries every `VoxelStream/R0Entry*` series (custom stats are NOT under a
`GameThread/` segment; the analyzer was corrected for that). The OFF capture has none.

Measurement window (488 frames, 17 with an R0 entry), per `analyze_r0_entry_profile.py`:

| slice     | total ms | calls | per-entry median | max   |
|-----------|----------|-------|------------------|-------|
| Entry     | 2140.5   | 17    | 114.1            | 382.6 |
| Footprint | 2131.5   | 2882  | 113.5            | 381.9 |
| Memo      | 2116.8   | 2882  | 112.8            | 380.9 |
| Compute   | 2114.9   | 484   | 112.7            | 380.8 |
| Resolve   | 2073.3   | 484   | 110.7            | 371.0 |
| Sky       | 0.1      | 2882  |                  |       |
| Nearest   | 8.4      | 17    | 0.55             | 0.88  |

Counters: ZCells 34,939, Evaluations 34,939, MemoHits 2,398, EpochInvalidations 0.
Nesting Resolve <= Compute <= Memo <= Footprint <= Entry held on every frame at 0.05 ms
tolerance; Entry residual (Entry - Footprint - Nearest) totals 0.58 ms.

Reading: within an R0 entry, essentially all game-thread time (97%) is `Resolve`, the
exact-asset resolve performed for each cold XY memo fill (484 fills, ~4.4 ms each). Memo
bookkeeping, Z predicates, sky and nearest are negligible. This is the README's first
candidate ("cold ComputeFootprintChunkZRange calls ResolvedAssetsForFootprint for every
XY memo fill") measured rather than suspected. The cold start before the measurement
window was one 3.99 s entry with 1,282 footprints, 3.92 s of it Resolve
(`five-second-windows.log.txt`).

Diagnostic overhead (29 OFF -> 28 ON, `r0-overhead-comparison.json`): settled-phase medians
within +-2% on frame, game-thread and GPU time; hitch tails move both ways (one pair,
noisy). No behaviour change when disabled is by construction (flag-gated).

Not claimed: any optimisation. Memoising resolves across Z chunks must preserve residency
epochs and request/error semantics; that is the next change to design, then A/B.
