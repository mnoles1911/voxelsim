# Staged R0 entry diagnostic (not applied or compiled)

Apply with git apply --ignore-space-change .scratch/r0-entry-profile/r0-entry-profile.patch (live file uses CRLF). Source SHA256 is recorded beside patch. No live source edited.

Enable -VoxelR0EntryProfile and existing -VoxelRecomputeCensus together. GT-only entry-slice CSV uses Accumulate, including split continuations. Five-second log prints maximum inclusive total for each slice, plus summed calls/counters. Maxima can belong to different slices; never add maxima. FEntry destructor flushes early returns; all scoped timers preserve callee returns. Disabled per-call timer construction does no clock read. Per-Z diagnostics use counters, not clocks.

Entry diagnostic begins after scan gates/setup (a small subset of existing full EntryMs); ends before existing close. Its children: Footprint encloses Memo, which encloses Compute, which encloses Resolve. Sky is another Footprint child. Nearest runs after sweep; RequestFootprint may nest below admission or Nearest. Do not sum inclusive scopes. Subtract only contemporaneous same-slice children when they are disjoint. Footprint minus Memo and Sky retains the range adjustment, Z predicates and candidate bookkeeping together. Existing census supplies record/park/budget outcome counts. Desired-cell geometry and carryover are Entry residual, not explicitly separately timed; GPU live-consumer tail is outside this R0 CPU entry bracket and intentionally not attributed to it.

Concrete candidates for follow-up, not proposed changes:
- Cold ComputeFootprintChunkZRange calls ResolvedAssetsForFootprint for every XY memo fill before testing the separate footprint-residency memo condition. Resolve cost/reuse is now directly distinguishable from terrain bounds and residency bookkeeping.
- AdmitCandidateCommit repeats IsFootprintResident on identical XY rectangles across eligible Z chunks; the failed-residency path also repeats RequestFootprint. Prefetch timer/count will reveal actual disk request work. An XY cache would require unchanged residency epoch and same request/error semantics; do not optimize without evidence.
- Memo hits still issue NoteFootprintZRange for permanent entries (GPU manager internally deduplicates). Memo total minus Compute includes these hits and cache/residency bookkeeping; counters distinguish hits and epoch invalidations.

Validation before acceptance: compile UE; run short enabled capture with early-return/split conditions; verify GT-only unique CSV labels, nonnegative times, Resolve <= Compute <= Memo <= Footprint <= Entry per slice (allow aggregate/float tolerance); compare diagnostic overhead with same scene disabled. This patch is diagnostic only and does not prove an optimization. It does not modify cache identity sources.

Full256 OFF/ON captures 24/25 are NOT accepted A/B: parent discovered runtime module changed during ON run. Do not infer size-culling gains from them.
